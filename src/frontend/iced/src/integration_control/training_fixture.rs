//! Shared inputs for rendered and unit training fixtures, never native admission policy.
use crate::generated::*;

pub(crate) fn metric_source(modelid: u64) -> TrainingMetricSource {
    TrainingMetricSource {
        scope: if modelid == 0 {
            TrainingRecordScope::SynchronizedSession
        } else {
            TrainingRecordScope::Model
        },
        modelid,
        weights: if modelid == 0 {
            EvaluatedWeights::Ordinary
        } else {
            EvaluatedWeights::Ema
        },
    }
}

pub(crate) fn unresolved_class_layout() -> ModelClassLayout {
    ModelClassLayout {
        version: 1,
        foreground: OrderedClassCatalog { names: Vec::new() },
        classnameevidence: OrderedClassCatalog { names: Vec::new() },
        slots: Vec::new(),
        scores: ClassScoreEncoding::SigmoidLogits,
        noobject: NoObjectEncoding::AllNegative,
        provenance: ClassLayoutProvenance {
            origin: ClassLayoutOrigin::Unresolved,
            producer: "fixture".into(),
            artifactsha256: String::new(),
        },
        supervisioninforegroundorder: false,
    }
}
