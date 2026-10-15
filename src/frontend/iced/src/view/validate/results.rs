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
fn cell_text(summary: Option<&MetricSummary>, index: usize) -> String {
    let value =
        values(summary)[index].map_or_else(|| "—".to_owned(), |value| format!("{value:.4}"));
    if (6..9).contains(&index) {
        summary.map_or(value.clone(), |summary| {
            format!("{value} @ {}", summary.detectionlimits[index - 6])
        })
    } else {
        value
    }
}
pub fn view<'a, Message: 'a>(model: &crate::view_model::ApplicationModel) -> Element<'a, Message> {
    let summary = model
        .workflow
        .validation
        .as_ref()
        .and_then(|snapshot| snapshot.metrics.as_ref());
    let boxes = summary.map(|summary| &summary.bbox);
    let masks = summary.and_then(|summary| summary.mask.as_ref());
    let keypoints = summary.and_then(|summary| summary.keypoint.as_ref());
    let columns = [("Boxes", boxes), ("Masks", masks), ("OKS", keypoints)];
    let mut heading = row![container(text("COCO").size(12)).width(Fill)];
    for (label, summary) in columns {
        if label == "Boxes" || summary.is_some() {
            heading = heading.push(container(text(label).size(12)).width(100));
        }
    }
    let mut rows = column![heading].spacing(3);
    for (index, name) in ROW_NAMES.iter().enumerate() {
        let mut cells = row![container(text(*name).size(12)).width(Fill)];
        for (label, summary) in columns {
            if label != "Boxes" && summary.is_none() {
                continue;
            }
            let cell = container(text(cell_text(summary, index)).size(12)).width(100);
            cells = cells.push(if label == "OKS" && index == 0 {
                cell.id("validate.results.oks.ap")
            } else {
                cell
            });
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
    fn recall_cells_keep_each_native_metric_limit_and_missing_value() {
        let mut boxes = MetricSummary {
            ap: 0.5,
            ap50: 0.6,
            ap75: 0.4,
            available: true,
            averagerecall: [Some(0.2), Some(0.4), None],
            detectionlimits: [1, 10, 300],
            areaap: [None; 3],
            areaar: [None; 3],
            confidencethreshold: 0.5,
            confidence: crate::generated::ConfidenceMetrics {
                precision: 0.5,
                recall: 0.5,
                f1: 0.5,
            },
        };
        let mut joints = boxes.clone();
        joints.detectionlimits = [20, 20, 20];
        assert_eq!(cell_text(Some(&boxes), 6), "0.2000 @ 1");
        assert_eq!(cell_text(Some(&boxes), 7), "0.4000 @ 10");
        assert_eq!(cell_text(Some(&boxes), 8), "— @ 300");
        assert_eq!(cell_text(Some(&joints), 6), "0.2000 @ 20");
        assert_eq!(cell_text(Some(&joints), 8), "— @ 20");
        boxes.available = false;
        assert_eq!(cell_text(Some(&boxes), 6), "— @ 1");
        assert_eq!(cell_text(None, 6), "—");
        assert_eq!(cell_text(Some(&joints), 0), "0.5000");
    }
    #[test]
    fn unavailable_coco_values_are_missing_and_the_summary_has_twelve_rows() {
        assert_eq!(values(None), [None; 12]);
        assert_eq!(ROW_NAMES.len(), 12);
        assert_eq!(&ROW_NAMES[3..6], &["AP small", "AP medium", "AP large"]);
    }
}
