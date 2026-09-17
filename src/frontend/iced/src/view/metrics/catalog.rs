//! Visual grouping only: values and identities are native generated declarations.
use crate::generated::{MetricSummary, MetricSummaryField, TrainingScalarsField};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Chart {
    Loss, Ap50, Ap, AverageRecall, Confidence, LearningRate,
    Components, Ap75, Errors, Mask, Area,
}
impl Chart {
    pub const ALL: [Self; 11] = [Self::Loss, Self::Ap50, Self::Ap, Self::AverageRecall,
        Self::Confidence, Self::LearningRate, Self::Components, Self::Ap75,
        Self::Errors, Self::Mask, Self::Area];
    pub fn label(self) -> &'static str {
        match self {
            Self::Loss => "Training loss", Self::Ap50 => "AP50", Self::Ap => "AP50:95",
            Self::AverageRecall => "Average recall", Self::Confidence => "Precision / recall / F1",
            Self::LearningRate => "Learning rates", Self::Components => "Loss components",
            Self::Ap75 => "AP75", Self::Errors => "Class / cardinality errors",
            Self::Mask => "Mask metrics", Self::Area => "Area breakdowns",
        }
    }
    pub fn loss(self) -> bool { matches!(self, Self::Loss | Self::Components) }
    pub fn main(self) -> bool { Self::ALL[..6].contains(&self) }
}
#[derive(Clone, Copy)]
pub(super) enum Source {
    Scalar(TrainingScalarsField),
    Evaluation { mask: bool, field: MetricSummaryField, index: usize },
}
pub(super) struct Metric {
    pub chart: Chart,
    pub label: String,
    pub source: Source,
}
pub(super) fn catalog() -> Vec<Metric> {
    use TrainingScalarsField::*;
    let mut metrics = crate::generated::TrainingScalars::FIELDS.iter().filter_map(|(field, name)| {
        let chart = match field {
            Total => Chart::Loss,
            LearningRate | LearningRateMin | LearningRateMax => Chart::LearningRate,
            ClassError | CardinalityError => Chart::Errors,
            ImagesPerSecond => return None,
            Classification | L1 | Giou | MaskCe | MaskDice | AuxiliaryWeighted
            | DenoisingWeighted | CorrespondenceWeighted => Chart::Components,
        };
        Some(Metric { chart, label: name.replace('_', " "), source: Source::Scalar(*field) })
    }).collect::<Vec<_>>();
    for mask in [false, true] {
        let prefix = if mask { "Mask" } else { "Box" };
        for (field, chart, label) in [
            (MetricSummaryField::Ap, Chart::Ap, "AP50:95"),
            (MetricSummaryField::Ap50, Chart::Ap50, "AP50"),
            (MetricSummaryField::Ap75, Chart::Ap75, "AP75"),
        ] {
            metrics.push(Metric { chart: if mask { Chart::Mask } else { chart },
                label: format!("{prefix} {label}"), source: Source::Evaluation { mask, field, index: 0 } });
        }
        for index in 0..3 {
            for (field, chart, label) in [
                (MetricSummaryField::AverageRecall, Chart::AverageRecall, "AR"),
                (MetricSummaryField::Confidence, Chart::Confidence, ["precision", "recall", "F1"][index]),
                (MetricSummaryField::AreaAp, Chart::Area, "AP"),
                (MetricSummaryField::AreaAr, Chart::Area, "AR"),
            ] {
                let suffix = if matches!(field, MetricSummaryField::AreaAp | MetricSummaryField::AreaAr) {
                    [" small", " medium", " large"][index]
                } else { "" };
                metrics.push(Metric { chart: if mask && chart != Chart::Area { Chart::Mask } else { chart },
                    label: format!("{prefix} {label}{suffix}"), source: Source::Evaluation { mask, field, index } });
            }
        }
    }
    metrics
}
pub(super) fn evaluation_value(summary: &MetricSummary, field: MetricSummaryField, index: usize) -> Option<f64> {
    if !summary.available { return None; }
    match field {
        MetricSummaryField::Ap => Some(summary.ap),
        MetricSummaryField::Ap50 => Some(summary.ap50),
        MetricSummaryField::Ap75 => Some(summary.ap75),
        MetricSummaryField::AverageRecall => summary.averagerecall[index],
        MetricSummaryField::AreaAp => summary.areaap[index],
        MetricSummaryField::AreaAr => summary.areaar[index],
        MetricSummaryField::Confidence => Some([summary.confidence.precision, summary.confidence.recall, summary.confidence.f1][index]),
        MetricSummaryField::Available | MetricSummaryField::DetectionLimits
        | MetricSummaryField::ConfidenceThreshold => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_native_scalar_has_the_intended_visual_policy() {
        use TrainingScalarsField::*;
        let expected = [
            (Total, Some(Chart::Loss)),
            (Classification, Some(Chart::Components)),
            (L1, Some(Chart::Components)),
            (Giou, Some(Chart::Components)),
            (MaskCe, Some(Chart::Components)),
            (MaskDice, Some(Chart::Components)),
            (AuxiliaryWeighted, Some(Chart::Components)),
            (DenoisingWeighted, Some(Chart::Components)),
            (CorrespondenceWeighted, Some(Chart::Components)),
            (ClassError, Some(Chart::Errors)),
            (CardinalityError, Some(Chart::Errors)),
            (LearningRate, Some(Chart::LearningRate)),
            (LearningRateMin, Some(Chart::LearningRate)),
            (LearningRateMax, Some(Chart::LearningRate)),
            (ImagesPerSecond, None),
        ];
        let metrics = catalog();
        assert_eq!(expected.len(), crate::generated::TrainingScalars::FIELDS.len());
        for (field, _) in crate::generated::TrainingScalars::FIELDS {
            let expected_chart = expected.iter().find(|(candidate, _)| candidate == &field).unwrap().1;
            let mut selected = metrics.iter().filter(|metric| matches!(metric.source, Source::Scalar(candidate) if candidate == field));
            assert_eq!(selected.next().map(|metric| metric.chart), expected_chart);
            assert!(selected.next().is_none());
        }
        // The closed visual selection has neither validation loss nor any
        // training-progress Y chart. Optional families retain their own cards.
        assert_eq!(Chart::ALL, [Chart::Loss, Chart::Ap50, Chart::Ap, Chart::AverageRecall,
            Chart::Confidence, Chart::LearningRate, Chart::Components, Chart::Ap75,
            Chart::Errors, Chart::Mask, Chart::Area]);
    }

    #[test]
    fn every_summary_identity_has_explicit_projection_or_metadata_policy() {
        use MetricSummaryField::*;
        let expected = [
            (Ap, 0, Some(0.42)), (Ap50, 0, Some(0.65)), (Ap75, 0, Some(0.37)),
            (Available, 0, None),
            (AverageRecall, 0, Some(0.2)), (AverageRecall, 1, Some(0.4)), (AverageRecall, 2, Some(0.6)),
            (DetectionLimits, 0, None),
            (AreaAp, 0, Some(0.1)), (AreaAp, 1, None), (AreaAp, 2, Some(0.7)),
            (AreaAr, 0, Some(0.2)), (AreaAr, 1, None), (AreaAr, 2, Some(0.8)),
            (Confidence, 0, Some(0.8)), (Confidence, 1, Some(0.5)), (Confidence, 2, Some(0.615)),
            (ConfidenceThreshold, 0, None),
        ];
        let mut summary = super::super::tests::evaluation().bbox;
        for (field, index, value) in expected {
            assert_eq!(evaluation_value(&summary, field, index), value, "{field:?}[{index}]");
        }
        let metrics = catalog();
        for metric in &metrics {
            if let Source::Evaluation { mask, field, .. } = metric.source {
                let expected_chart = match field {
                    Ap => if mask { Chart::Mask } else { Chart::Ap },
                    Ap50 => if mask { Chart::Mask } else { Chart::Ap50 },
                    Ap75 => if mask { Chart::Mask } else { Chart::Ap75 },
                    AverageRecall => if mask { Chart::Mask } else { Chart::AverageRecall },
                    Confidence => if mask { Chart::Mask } else { Chart::Confidence },
                    AreaAp | AreaAr => Chart::Area,
                    Available | DetectionLimits | ConfidenceThreshold => panic!("metadata entered the plotted catalog"),
                };
                assert_eq!(metric.chart, expected_chart);
            }
        }
        summary.available = false;
        for (field, index, _) in expected {
            assert_eq!(evaluation_value(&summary, field, index), None);
        }
    }
}
