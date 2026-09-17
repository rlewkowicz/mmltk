//! Visual grouping only: values and identities are native generated declarations.
use crate::generated::{
    ConfidenceMetricsField, MetricSummary, MetricSummaryField, TrainingScalarsField,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Chart {
    Loss,
    Ap50,
    Ap,
    AverageRecall,
    Confidence,
    LearningRate,
    Components,
    Ap75,
    Errors,
    Mask,
    Area,
}
impl Chart {
    pub const ALL: [Self; 11] = [
        Self::Loss,
        Self::Ap50,
        Self::Ap,
        Self::AverageRecall,
        Self::Confidence,
        Self::LearningRate,
        Self::Components,
        Self::Ap75,
        Self::Errors,
        Self::Mask,
        Self::Area,
    ];
    pub fn label(self) -> &'static str {
        match self {
            Self::Loss => "Training loss",
            Self::Ap50 => "AP50",
            Self::Ap => "AP50:95",
            Self::AverageRecall => "Average recall",
            Self::Confidence => "Precision / recall / F1",
            Self::LearningRate => "Learning rates",
            Self::Components => "Loss components",
            Self::Ap75 => "AP75",
            Self::Errors => "Class / cardinality errors",
            Self::Mask => "Mask metrics",
            Self::Area => "Area breakdowns",
        }
    }
    pub fn loss(self) -> bool {
        matches!(self, Self::Loss | Self::Components)
    }
    pub fn main(self) -> bool {
        Self::ALL[..6].contains(&self)
    }
}
#[derive(Clone, Copy)]
pub(super) enum Source {
    Scalar(TrainingScalarsField),
    Evaluation {
        mask: bool,
        field: MetricSummaryField,
    },
}
pub(super) struct Metric {
    pub chart: Chart,
    pub label: String,
    pub source: Source,
}
pub(super) fn catalog() -> Vec<Metric> {
    use TrainingScalarsField::*;
    let mut metrics = crate::generated::TrainingScalars::FIELDS
        .iter()
        .filter_map(|(field, name)| {
            let chart = match field {
                Total => Chart::Loss,
                LearningRate | LearningRateMin | LearningRateMax => Chart::LearningRate,
                ClassError | CardinalityError => Chart::Errors,
                ImagesPerSecond => return None,
                Classification
                | L1
                | Giou
                | MaskCe
                | MaskDice
                | AuxiliaryWeighted
                | DenoisingWeighted
                | CorrespondenceWeighted => Chart::Components,
            };
            Some(Metric {
                chart,
                label: name.replace('_', " "),
                source: Source::Scalar(*field),
            })
        })
        .collect::<Vec<_>>();
    for mask in [false, true] {
        let prefix = if mask { "Mask" } else { "Box" };
        let mut selected = MetricSummary::FIELDS
            .iter()
            .filter_map(|(field, _)| {
                let (order, chart, label) = evaluation_presentation(*field)?;
                Some((
                    order,
                    Metric {
                        chart: if mask && chart != Chart::Area {
                            Chart::Mask
                        } else {
                            chart
                        },
                        label: format!("{prefix} {label}"),
                        source: Source::Evaluation {
                            mask,
                            field: *field,
                        },
                    },
                ))
            })
            .collect::<Vec<_>>();
        selected.sort_by_key(|(order, _)| *order);
        metrics.extend(selected.into_iter().map(|(_, metric)| metric));
    }
    metrics
}
// Exhaustive visual policy, deliberately independent of native declaration order.
// The interleaved order preserves existing series/color identities.
fn evaluation_presentation(field: MetricSummaryField) -> Option<((usize, usize), Chart, String)> {
    use MetricSummaryField::*;
    let (order, chart, label) = match field {
        Ap => ((0, 0), Chart::Ap, "AP50:95".into()),
        Ap50 => ((0, 1), Chart::Ap50, "AP50".into()),
        Ap75 => ((0, 2), Chart::Ap75, "AP75".into()),
        AverageRecall(index) => ((index + 1, 0), Chart::AverageRecall, "AR".into()),
        Confidence(field) => {
            let (order, label) = match field {
                ConfidenceMetricsField::Precision => (0, "precision"),
                ConfidenceMetricsField::Recall => (1, "recall"),
                ConfidenceMetricsField::F1 => (2, "F1"),
            };
            ((order + 1, 1), Chart::Confidence, label.into())
        }
        AreaAp(index) | AreaAr(index) => {
            let ap = matches!(field, AreaAp(_));
            let area = match index {
                0 => "small".into(),
                1 => "medium".into(),
                2 => "large".into(),
                _ => format!("area {index}"),
            };
            (
                (index + 1, if ap { 2 } else { 3 }),
                Chart::Area,
                format!("{} {area}", if ap { "AP" } else { "AR" }),
            )
        }
        Available | DetectionLimits(_) | ConfidenceThreshold => return None,
    };
    Some((order, chart, label))
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
        assert_eq!(
            expected.len(),
            crate::generated::TrainingScalars::FIELDS.len()
        );
        for (field, _) in crate::generated::TrainingScalars::FIELDS {
            let expected_chart = expected
                .iter()
                .find(|(candidate, _)| candidate == &field)
                .unwrap()
                .1;
            let mut selected = metrics.iter().filter(
                |metric| matches!(metric.source, Source::Scalar(candidate) if candidate == field),
            );
            assert_eq!(selected.next().map(|metric| metric.chart), expected_chart);
            assert!(selected.next().is_none());
        }
        // The closed visual selection has neither validation loss nor any
        // training-progress Y chart. Optional families retain their own cards.
        assert_eq!(
            Chart::ALL,
            [
                Chart::Loss,
                Chart::Ap50,
                Chart::Ap,
                Chart::AverageRecall,
                Chart::Confidence,
                Chart::LearningRate,
                Chart::Components,
                Chart::Ap75,
                Chart::Errors,
                Chart::Mask,
                Chart::Area
            ]
        );
    }

    #[test]
    fn every_summary_identity_has_explicit_projection_or_metadata_policy() {
        use ConfidenceMetricsField::{F1, Precision, Recall};
        use MetricSummaryField::*;
        let expected = [
            (Ap, Some(0.42)),
            (Ap50, Some(0.65)),
            (Ap75, Some(0.37)),
            (AverageRecall(0), Some(0.2)),
            (AverageRecall(1), Some(0.4)),
            (AverageRecall(2), Some(0.6)),
            (AreaAp(0), Some(0.1)),
            (AreaAp(1), None),
            (AreaAp(2), Some(0.7)),
            (AreaAr(0), Some(0.2)),
            (AreaAr(1), None),
            (AreaAr(2), Some(0.8)),
            (Confidence(Precision), Some(0.8)),
            (Confidence(Recall), Some(0.5)),
            (Confidence(F1), Some(0.615)),
        ];
        let summary = super::super::tests::evaluation().bbox;
        let metrics = catalog();
        for (field, value) in expected {
            assert_eq!(summary.value(field), value, "{field:?}");
        }
        for (field, _) in MetricSummary::FIELDS {
            let selected = metrics.iter().filter(|m| matches!(m.source, Source::Evaluation { mask: false, field: candidate } if candidate == field)).count();
            assert_eq!(
                selected,
                usize::from(evaluation_presentation(field).is_some())
            );
        }
        for metric in &metrics {
            if let Source::Evaluation { mask, field } = metric.source {
                let expected_chart = match field {
                    Ap => {
                        if mask {
                            Chart::Mask
                        } else {
                            Chart::Ap
                        }
                    }
                    Ap50 => {
                        if mask {
                            Chart::Mask
                        } else {
                            Chart::Ap50
                        }
                    }
                    Ap75 => {
                        if mask {
                            Chart::Mask
                        } else {
                            Chart::Ap75
                        }
                    }
                    AverageRecall(_) => {
                        if mask {
                            Chart::Mask
                        } else {
                            Chart::AverageRecall
                        }
                    }
                    Confidence(_) => {
                        if mask {
                            Chart::Mask
                        } else {
                            Chart::Confidence
                        }
                    }
                    AreaAp(_) | AreaAr(_) => Chart::Area,
                    Available | DetectionLimits(_) | ConfidenceThreshold => {
                        panic!("metadata entered the plotted catalog")
                    }
                };
                assert_eq!(metric.chart, expected_chart);
            }
        }
        assert_eq!(
            metrics
                .iter()
                .filter(|m| matches!(m.source, Source::Evaluation { mask: false, .. }))
                .count(),
            expected.len()
        );
        for field in [Available, DetectionLimits(0), ConfidenceThreshold] {
            assert!(evaluation_presentation(field).is_none());
        }
        for field in [
            AverageRecall(usize::MAX),
            AreaAp(usize::MAX),
            AreaAr(usize::MAX),
            DetectionLimits(usize::MAX),
        ] {
            assert_eq!(summary.value(field), None);
        }
        let selected = metrics
            .iter()
            .filter_map(|m| match m.source {
                Source::Evaluation { mask: false, field } => Some(field),
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(
            selected,
            [
                Ap,
                Ap50,
                Ap75,
                AverageRecall(0),
                Confidence(Precision),
                AreaAp(0),
                AreaAr(0),
                AverageRecall(1),
                Confidence(Recall),
                AreaAp(1),
                AreaAr(1),
                AverageRecall(2),
                Confidence(F1),
                AreaAp(2),
                AreaAr(2)
            ]
        );
    }
}
