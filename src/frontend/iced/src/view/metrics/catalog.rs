//! Visual grouping only: values and identities are native generated declarations.
use crate::generated::{
    ConfidenceMetricsField, MetricSummary, MetricSummaryField, TrainingScalarsField,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Chart {
    Loss,
    Ap,
    AverageRecall,
    Confidence,
    LearningRate,
    Errors,
    Area,
    Throughput,
    Gpu,
}
impl Chart {
    pub const ALL: [Self; 9] = [
        Self::Loss,
        Self::Ap,
        Self::AverageRecall,
        Self::Confidence,
        Self::LearningRate,
        Self::Errors,
        Self::Area,
        Self::Throughput,
        Self::Gpu,
    ];
    pub fn label(self) -> &'static str {
        match self {
            Self::Loss => "Loss",
            Self::Ap => "AP",
            Self::AverageRecall => "Average recall",
            Self::Confidence => "Precision / recall / F1",
            Self::LearningRate => "Learning rates",
            Self::Errors => "Class / cardinality errors",
            Self::Area => "Area breakdowns",
            Self::Throughput => "Images / second",
            Self::Gpu => "GPU",
        }
    }
    pub fn loss(self) -> bool {
        self == Self::Loss
    }
    pub fn main(self) -> bool {
        Self::ALL[..5].contains(&self)
    }
    pub fn performance(self) -> bool {
        matches!(self, Self::Throughput | Self::Gpu)
    }
}

#[derive(Clone, Copy)]
pub(super) enum Source {
    Scalar(TrainingScalarsField),
    Evaluation {
        kind: crate::generated::EvaluationMetricKind,
        field: MetricSummaryField,
    },
}
pub(super) struct Metric {
    pub chart: Chart,
    pub label: String,
    pub source: Source,
}
pub(super) fn catalog() -> Vec<Metric> {
    let (mut metrics, performance): (Vec<_>, Vec<_>) = crate::generated::TrainingScalars::FIELDS
        .iter()
        .map(|(field, name)| {
            let chart = scalar_chart(*field);
            let label = match field {
                TrainingScalarsField::ImagesPerSecond => "Epoch average images / second".into(),
                TrainingScalarsField::GpuUtilizationPercent => "GPU utilization (%)".into(),
                TrainingScalarsField::GpuMemoryUsedBytes => "GPU memory used (GiB)".into(),
                TrainingScalarsField::GpuMemoryTotalBytes => "GPU memory total (GiB)".into(),
                TrainingScalarsField::GpuTemperatureCelsius => "GPU temperature (°C)".into(),
                TrainingScalarsField::Total => "Total (epoch mean)".into(),
                TrainingScalarsField::Classification => "Classification".into(),
                TrainingScalarsField::L1 => "Box L1".into(),
                TrainingScalarsField::Giou => "Box GIoU".into(),
                TrainingScalarsField::MaskCe => "Mask CE".into(),
                TrainingScalarsField::MaskDice => "Mask Dice".into(),
                TrainingScalarsField::KeypointL1 => "Keypoint L1".into(),
                TrainingScalarsField::KeypointFindable => "Keypoint findability".into(),
                TrainingScalarsField::KeypointVisible => "Keypoint visibility".into(),
                TrainingScalarsField::KeypointNll => "Keypoint NLL".into(),
                TrainingScalarsField::AuxiliaryWeighted => "Auxiliary (weighted)".into(),
                TrainingScalarsField::DenoisingWeighted => "Denoising (weighted)".into(),
                TrainingScalarsField::CorrespondenceWeighted => "Correspondence (weighted)".into(),
                _ => name.replace('_', " "),
            };
            Metric {
                chart,
                label,
                source: Source::Scalar(*field),
            }
        })
        .partition(|metric| !metric.chart.performance());
    let (mut keypoints, mut metrics): (Vec<_>, Vec<_>) = metrics.into_iter().partition(|metric| matches!(metric.source, Source::Scalar(TrainingScalarsField::KeypointL1 | TrainingScalarsField::KeypointFindable | TrainingScalarsField::KeypointVisible | TrainingScalarsField::KeypointNll)));
    for kind in crate::generated::EVALUATION_METRIC_KIND_VALUES {
        let prefix = match kind { crate::generated::EvaluationMetricKind::Box => "Box", crate::generated::EvaluationMetricKind::Mask => "Mask", crate::generated::EvaluationMetricKind::Keypoint => "OKS" };
        let mut selected = MetricSummary::FIELDS
            .iter()
            .filter_map(|(field, _)| {
                let (order, chart, label) = evaluation_presentation(*field)?;
                Some((
                    order,
                    Metric {
                        chart,
                        label: format!("{prefix} {label}"),
                        source: Source::Evaluation {
                            kind: *kind,
                            field: *field,
                        },
                    },
                ))
            })
            .collect::<Vec<_>>();
        selected.sort_by_key(|(order, _)| *order);
        let destination = if *kind == crate::generated::EvaluationMetricKind::Keypoint { &mut keypoints } else { &mut metrics };
        destination.extend(selected.into_iter().map(|(_, metric)| metric));
    }
    // Keep the metric inventory and color indices stable across chart grouping.
    metrics.extend(performance);
    metrics.extend(keypoints);
    metrics
}
fn scalar_chart(field: TrainingScalarsField) -> Chart {
    use TrainingScalarsField::*;
    match field {
        Total => Chart::Loss,
        LearningRate | LearningRateMin | LearningRateMax => Chart::LearningRate,
        ClassError | CardinalityError => Chart::Errors,
        ImagesPerSecond => Chart::Throughput,
        GpuUtilizationPercent => Chart::Gpu,
        GpuMemoryUsedBytes | GpuMemoryTotalBytes => Chart::Gpu,
        GpuTemperatureCelsius => Chart::Gpu,
        Classification
        | L1
        | Giou
        | MaskCe
        | MaskDice
        | AuxiliaryWeighted
        | DenoisingWeighted
        | CorrespondenceWeighted | KeypointL1 | KeypointFindable | KeypointVisible | KeypointNll => Chart::Loss,
    }
}
// Exhaustive visual policy, deliberately independent of native declaration order.
// The interleaved order preserves existing series/color identities.
fn evaluation_presentation(field: MetricSummaryField) -> Option<((usize, usize), Chart, String)> {
    use MetricSummaryField::*;
    let (order, chart, label) = match field {
        Ap => ((0, 0), Chart::Ap, "AP50:95".into()),
        Ap50 => ((0, 1), Chart::Ap, "AP50".into()),
        Ap75 => ((0, 2), Chart::Ap, "AP75".into()),
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
            (Classification, Some(Chart::Loss)),
            (L1, Some(Chart::Loss)),
            (Giou, Some(Chart::Loss)),
            (MaskCe, Some(Chart::Loss)),
            (MaskDice, Some(Chart::Loss)),
            (KeypointL1, Some(Chart::Loss)),
            (KeypointFindable, Some(Chart::Loss)),
            (KeypointVisible, Some(Chart::Loss)),
            (KeypointNll, Some(Chart::Loss)),
            (AuxiliaryWeighted, Some(Chart::Loss)),
            (DenoisingWeighted, Some(Chart::Loss)),
            (CorrespondenceWeighted, Some(Chart::Loss)),
            (ClassError, Some(Chart::Errors)),
            (CardinalityError, Some(Chart::Errors)),
            (LearningRate, Some(Chart::LearningRate)),
            (LearningRateMin, Some(Chart::LearningRate)),
            (LearningRateMax, Some(Chart::LearningRate)),
            (ImagesPerSecond, Some(Chart::Throughput)),
            (GpuUtilizationPercent, Some(Chart::Gpu)),
            (GpuMemoryUsedBytes, Some(Chart::Gpu)),
            (GpuMemoryTotalBytes, Some(Chart::Gpu)),
            (GpuTemperatureCelsius, Some(Chart::Gpu)),
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
        assert_eq!(
            Chart::ALL
                .into_iter()
                .filter(|chart| chart.main())
                .collect::<Vec<_>>(),
            [
                Chart::Loss,
                Chart::Ap,
                Chart::AverageRecall,
                Chart::Confidence,
                Chart::LearningRate
            ]
        );
        assert_eq!(
            Chart::ALL
                .into_iter()
                .filter(|chart| chart.performance())
                .collect::<Vec<_>>(),
            [Chart::Throughput, Chart::Gpu]
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
            let selected = metrics.iter().filter(|m| matches!(m.source, Source::Evaluation { kind: crate::generated::EvaluationMetricKind::Box, field: candidate } if candidate == field)).count();
            assert_eq!(
                selected,
                usize::from(evaluation_presentation(field).is_some())
            );
        }
        for metric in &metrics {
            if let Source::Evaluation { field, .. } = metric.source {
                let expected_chart = match field {
                    Ap | Ap50 | Ap75 => Chart::Ap,
                    AverageRecall(_) => Chart::AverageRecall,
                    Confidence(_) => Chart::Confidence,
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
                .filter(|m| matches!(m.source, Source::Evaluation { kind: crate::generated::EvaluationMetricKind::Box, .. }))
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
                Source::Evaluation { kind: crate::generated::EvaluationMetricKind::Box, field } => Some(field),
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
