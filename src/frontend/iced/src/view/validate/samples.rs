use crate::fluent_theme::Element;
use crate::generated::{ValidationOverlays, ValidationSampleIdentity};
use crate::presentation_surface::{self, Surface};
use crate::view::workflow::overlay_controls;
use iced::widget::{button, column, container, row, text};
#[derive(Debug, Clone)]
pub enum Message {
    Select(ValidationSampleIdentity),
    Close,
    Fit,
    Labels(bool, bool),
    Overlays(ValidationOverlays),
}
pub struct Component {
    ground_truth: bool,
    prediction: bool,
    fit_revision: u64,
}
impl Default for Component {
    fn default() -> Self {
        Self {
            ground_truth: true,
            prediction: true,
            fit_revision: 0,
        }
    }
}
impl Component {
    pub fn update(&mut self, message: &Message) -> bool {
        match *message {
            Message::Labels(ground_truth, value) => {
                if ground_truth {
                    self.ground_truth = value;
                } else {
                    self.prediction = value;
                }
                true
            }
            Message::Fit => {
                self.fit_revision = self.fit_revision.wrapping_add(1);
                true
            }
            _ => false,
        }
    }
    pub fn view<'a>(
        &self,
        surface: Option<Surface>,
        model: &crate::view_model::ApplicationModel,
        settings: &crate::view::settings::SettingsModel,
        width: f32,
    ) -> Element<'a, Message> {
        let paired = surface.and_then(presentation_surface::drawable_validation);
        let Some((surface, content)) = paired else {
            return column![text("Validation samples"), empty_atlas(width)]
                .spacing(6)
                .into();
        };
        let metadata = &content.metadata;
        let available = model.settings_edit_available()
            && !model
                .has_pending(crate::generated::ApplicationIntentEndpoint::ValidationSetOverlays);
        let controls = |ground_truth: bool| {
            let overlays = metadata.overlays.clone();
            let (boxes, masks, labels, ids) = if ground_truth {
                (
                    overlays.groundtruthboxes,
                    overlays.groundtruthmasks,
                    self.ground_truth,
                    [
                        "validate.gt.labels",
                        "validate.gt.masks",
                        "validate.gt.boxes",
                    ],
                )
            } else {
                (
                    overlays.predictionboxes,
                    overlays.predictionmasks,
                    self.prediction,
                    [
                        "validate.pred.labels",
                        "validate.pred.masks",
                        "validate.pred.boxes",
                    ],
                )
            };
            overlay_controls::view(labels, masks, boxes, true, available, ids).map(move |message| {
                let mut next = overlays.clone();
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
            })
        };
        let labels = presentation_surface::labels::Source::Validation(
            content.clone(),
            self.ground_truth,
            self.prediction,
        );
        let image: Element<'a, Message> = if metadata.detail {
            let mut shown = surface;
            shown.fit_revision = self.fit_revision;
            column![
                crate::view::image_viewer::controls(
                    Message::Fit,
                    Message::Close,
                    "validate.detail.fit",
                    "validate.detail.close"
                ),
                container(crate::view::image_viewer::image(
                    shown,
                    labels,
                    None,
                    crate::workspace_fps::enabled(settings),
                    "validate.detail.image"
                ))
                .id("validate.detail.image")
                .height(width.max(1.0)),
            ]
            .spacing(6)
            .into()
        } else {
            let mut rows = column![].spacing(4);
            const IDS: [&str; 6] = [
                "validate.sample.0",
                "validate.sample.1",
                "validate.sample.2",
                "validate.sample.3",
                "validate.sample.4",
                "validate.sample.5",
            ];
            for first in [0, 3] {
                let mut cells = row![].spacing(4);
                for index in first..first + 3 {
                    let sample = &metadata.samples[index];
                    let cell: Element<'_, Message> = if sample.available {
                        let mut crop = surface;
                        crop.crop = Some([
                            sample.crop.x,
                            sample.crop.y,
                            sample.crop.width,
                            sample.crop.height,
                        ]);
                        crop.viewer_identity = Some((
                            sample.identity.generation,
                            u64::from(sample.identity.datasetindex),
                        ));
                        button(crate::view::image_viewer::image(
                            crop,
                            labels.clone(),
                            None,
                            false,
                            IDS[index],
                        ))
                        .padding(0)
                        .on_press(Message::Select(sample.identity.clone()))
                        .into()
                    } else {
                        container(text("No sample")).center(iced::Fill).into()
                    };
                    cells = cells.push(
                        container(cell)
                            .id(IDS[index])
                            .width(iced::Length::FillPortion(1))
                            .height((width - 8.0).max(3.0) / 3.0),
                    );
                }
                rows = rows.push(cells);
            }
            rows.into()
        };
        column![
            text("Ground truth"),
            controls(true),
            text("Predictions"),
            controls(false),
            image
        ]
        .spacing(6)
        .into()
    }
}
fn empty_atlas(width: f32) -> Element<'static, Message> {
    [0, 1]
        .into_iter()
        .fold(column![].spacing(4), |rows, _| {
            rows.push([0, 1, 2].into_iter().fold(row![].spacing(4), |cells, _| {
                cells.push(
                    container(text("No sample"))
                        .center(iced::Fill)
                        .width(iced::Length::FillPortion(1))
                        .height((width - 8.0).max(3.0) / 3.0),
                )
            }))
        })
        .into()
}
