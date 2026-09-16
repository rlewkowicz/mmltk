use crate::fluent_theme::Element;
use crate::generated::{self, EvaluationDetailQuery};
use iced::widget::{button, column, row, text};
#[derive(Debug, Clone)]
pub enum Message {
    Page(EvaluationDetailQuery),
    Iou(usize),
    Recall(usize),
}
#[derive(Default)]
pub struct Component {
    iou: usize,
    recall: usize,
}
impl Component {
    pub fn update(&mut self, message: &Message) -> bool {
        let axes = &generated::EVALUATION_AXIS_CATALOG[0];
        match *message {
            Message::Iou(index) if index < axes.iou.len() => {
                self.iou = index;
                true
            }
            Message::Recall(index) if index < axes.recall.len() => {
                self.recall = index;
                true
            }
            Message::Page(_) => false,
            _ => true,
        }
    }
    pub fn view(&self, model: &crate::view_model::ApplicationModel) -> Element<'_, Message> {
        let Some(snapshot) = model.workflow.validation.as_ref() else {
            return text("Validation metrics unavailable").into();
        };
        let axes = &generated::EVALUATION_AXIS_CATALOG[0];
        let generation = snapshot.operation.generationfrontier;
        let page = model
            .workflow
            .validation_details
            .as_ref()
            .filter(|page| page.generation == generation);
        let request = |offset| {
            Message::Page(EvaluationDetailQuery {
                generation,
                offset,
                count: 4,
            })
        };
        let ready = snapshot.detailrows != 0
            && !model.has_pending(generated::ApplicationIntentEndpoint::ValidationDetails);
        let mut content = column![text("Metrics").size(18)];
        if let Some(summary) = &snapshot.metrics {
            for (name, metric) in std::iter::once(("Boxes", &summary.bbox))
                .chain(summary.mask.as_ref().map(|m| ("Masks", m)))
            {
                content = content.push(text(if metric.available {
                    format!("{name}: AP {:.4} · AP50 {:.4} · AP75 {:.4}\nPrecision {:.4} · recall {:.4} · F1 {:.4}\nDetection limits {:?}", metric.ap, metric.ap50, metric.ap75,
                        metric.confidence.precision, metric.confidence.recall, metric.confidence.f1, metric.detectionlimits)
                } else { format!("{name}: unavailable (no eligible ground truth)") }));
            }
        }
        content =
            content.push(
                row![
                    button("Previous").on_press_maybe(
                        page.filter(|page| ready && page.offset != 0)
                            .map(|page| request(page.offset.saturating_sub(4)))
                    ),
                    button(if page.is_none() { "Details" } else { "Next" }).on_press_maybe(
                        ready
                            .then(|| request(
                                page.map_or(0, |page| page.offset + page.rows.len() as u32)
                            ))
                            .filter(|_| page.is_none_or(|page| page.offset
                                + (page.rows.len() as u32)
                                < page.total))
                    ),
                ]
                .spacing(4),
            );
        content = content.push(
            row![
                button("− IoU").on_press_maybe(self.iou.checked_sub(1).map(Message::Iou)),
                text(format!("IoU {:.2}", axes.iou[self.iou])),
                button("+ IoU").on_press_maybe(
                    (self.iou + 1 < axes.iou.len()).then_some(Message::Iou(self.iou + 1))
                )
            ]
            .spacing(4),
        );
        content = content.push(
            row![
                button("− Recall").on_press_maybe(self.recall.checked_sub(1).map(Message::Recall)),
                text(format!("Recall {:.2}", axes.recall[self.recall])),
                button("+ Recall").on_press_maybe(
                    (self.recall + 1 < axes.recall.len())
                        .then_some(Message::Recall(self.recall + 1))
                )
            ]
            .spacing(4),
        );
        if let Some(page) = page {
            for metric in &page.rows {
                let name = metric
                    .categoryname
                    .as_ref()
                    .map_or("Overall", |name| name.value.as_str());
                content = content.push(text(format!("{name} · {:?} · {:?}\n{}", metric.kind, metric.area,
                    if metric.available { format!("AP {:.4} · recall {:?}\nPR precision {:.4}\nPrecision {:.4} · recall {:.4} · F1 {:.4}\nConfidence ≥ {:.4} · limits {:?}",
                        metric.averageprecision[self.iou], metric.averagerecall.map(|row| row[self.iou]), metric.precisioncurve[self.iou][self.recall],
                        metric.confidence.precision, metric.confidence.recall, metric.confidence.f1, metric.confidencethreshold, metric.detectionlimits)
                    } else { "Unavailable".into() })));
            }
        }
        content.spacing(8).into()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn metric_axis_selection_is_bounded_by_the_generated_catalog() {
        let mut component = Component::default();
        let axes = &generated::EVALUATION_AXIS_CATALOG[0];
        component.update(&Message::Iou(axes.iou.len() - 1));
        component.update(&Message::Recall(axes.recall.len() - 1));
        component.update(&Message::Iou(axes.iou.len()));
        component.update(&Message::Recall(usize::MAX));
        assert_eq!(component.iou, axes.iou.len() - 1);
        assert_eq!(component.recall, axes.recall.len() - 1);
        assert!(!component.update(&Message::Page(EvaluationDetailQuery {
            generation: 7,
            offset: 4,
            count: 4
        })));
    }
}
