use crate::fluent_theme::Element;
use crate::generated::MetricSummary;
use iced::Fill;
use iced::widget::{column, container, row, text};

pub const ROW_NAMES: [&str; 12] = [
    "AP 50:95",
    "AP50",
    "AP75",
    "AP small",
    "AP medium",
    "AP large",
    "AR",
    "AR",
    "AR",
    "AR small",
    "AR medium",
    "AR large",
];
fn values(summary: Option<&MetricSummary>) -> [Option<f64>; 12] {
    let Some(value) = summary.filter(|value| value.available) else {
        return [None; 12];
    };
    [
        Some(value.ap),
        Some(value.ap50),
        Some(value.ap75),
        value.areaap[0],
        value.areaap[1],
        value.areaap[2],
        value.averagerecall[0],
        value.averagerecall[1],
        value.averagerecall[2],
        value.areaar[0],
        value.areaar[1],
        value.areaar[2],
    ]
}
pub fn view<'a, Message: 'a>(model: &crate::view_model::ApplicationModel) -> Element<'a, Message> {
    let summary = model
        .workflow
        .validation
        .as_ref()
        .and_then(|snapshot| snapshot.metrics.as_ref());
    let boxes = summary.map(|summary| &summary.bbox);
    let masks = summary.and_then(|summary| summary.mask.as_ref());
    let columns = [values(boxes), values(masks)];
    let mut heading = row![
        container(text("COCO").size(12)).width(Fill),
        container(text("Boxes").size(12)).width(66)
    ];
    if masks.is_some() {
        heading = heading.push(container(text("Masks").size(12)).width(66));
    }
    let mut rows = column![heading].spacing(3);
    for (index, name) in ROW_NAMES.iter().enumerate() {
        let name = if (6..9).contains(&index) {
            boxes.map_or_else(
                || "AR @ —".to_owned(),
                |summary| format!("AR @ {}", summary.detectionlimits[index - 6]),
            )
        } else {
            (*name).to_owned()
        };
        let mut cells = row![container(text(name).size(12)).width(Fill)];
        for values in columns.iter().take(if masks.is_some() { 2 } else { 1 }) {
            cells = cells.push(
                container(
                    text(
                        values[index].map_or_else(|| "—".to_owned(), |value| format!("{value:.4}")),
                    )
                    .size(12),
                )
                .width(66),
            );
        }
        rows = rows.push(container(cells).height(iced::Length::FillPortion(1)));
    }
    container(rows)
        .padding([4, 8])
        .width(Fill)
        .height(Fill)
        .into()
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unavailable_coco_values_are_missing_and_the_summary_has_twelve_rows() {
        assert_eq!(values(None), [None; 12]);
        assert_eq!(ROW_NAMES.len(), 12);
        assert_eq!(&ROW_NAMES[3..6], &["AP small", "AP medium", "AP large"]);
    }
}
