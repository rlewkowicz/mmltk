use super::model_selection::effective_model_selection;
use super::*;

pub(crate) fn bootstrapped() -> ApplicationModel {
    let mut model = ApplicationModel::default();
    let snapshots = crate::generated::application_snapshot_defaults()
        .expect("generated application defaults")
        .into_iter()
        .map(|fact| fact.value)
        .collect();
    model
        .install_bootstrap(crate::generated::SCHEMA_FINGERPRINT, snapshots)
        .expect("typed bootstrap");
    model
}

pub(crate) fn explore_snapshot() -> crate::generated::ExploreSnapshot {
    crate::generated::application_snapshot_defaults()
        .expect("generated application defaults")
        .into_iter()
        .find_map(|fact| match fact.value {
            crate::generated::ApplicationSnapshot::Explore(value) => Some(value),
            _ => None,
        })
        .expect("generated Explore snapshot")
}

pub(crate) fn visual_frame(kind: PresentationSourceKind, revision: u64) -> VisualFrame {
    VisualFrame {
        source: PresentationSourceIdentity { kind, instance: 1 },
        extent: VisualExtent {
            width: 640,
            height: 480,
        },
        revision,
        cleanrevision: revision,
        content: crate::generated::VisualRegion {
            x: 0,
            y: 0,
            width: 640,
            height: 480,
        },
    }
}

pub(crate) fn annotation_object(category: u16) -> crate::generated::AnnotationObject {
    use crate::generated::*;
    let point = AnnotationPoint { x: 1.0, y: 2.0 };
    let color = AnnotationColor {
        hue: 0.0,
        saturation: 1.0,
        value: 1.0,
    };
    let range = AnnotationColorRange {
        center: color.clone(),
        minus: color.clone(),
        plus: color,
        sampling: false,
    };
    AnnotationObject {
        name: AnnotationText::try_from("object").unwrap(),
        shape: AnnotationShape::Box,
        box_: AnnotationBox {
            first: point.clone(),
            second: AnnotationPoint { x: 8.0, y: 9.0 },
        },
        point,
        mask: AnnotationMask {
            runs: Vec::new(),
            cleanupradius: 0,
            cleanup: AnnotationMaskCleanup::LargestComponent,
            present: false,
        },
        sup: range.clone(),
        nosup: range,
        maskpoints: Vec::new(),
        splineknots: Vec::new(),
        skeletonnodes: Vec::new(),
        skeletonedges: Vec::new(),
        category,
        splineclosed: false,
        enabled: true,
    }
}

pub(super) fn accepted_train_model(model: &ApplicationModel) -> ModelUiState {
    let settings = &model
        .settings_snapshot
        .as_ref()
        .expect("Settings snapshot")
        .settingsstate;
    let train = &settings.workflows.train;
    let mut snapshot = model.model_snapshot.clone().expect("Model snapshot");
    snapshot.generation += 1;
    snapshot.active = false;
    snapshot.selection.key.workflow = FeatureId::Train;
    snapshot.selection.key.source = train.modelsource;
    snapshot.selection.key.input = train.modelinput;
    snapshot.selection.key.preset = train.request.presetname.clone();
    snapshot.selection.key.resolution = train.request.resolution as u32;
    snapshot.selection.artifact = crate::generated::RFDETR_PRESET_CATALOG
        .iter()
        .find(|preset| preset.presetname.as_ref() == train.request.presetname)
        .expect("generated preset")
        .canonicalweightfilename
        .to_string();
    snapshot.terminal.outcome = ModelSelectionOutcome::Accepted;
    snapshot.terminal.detail.clear();
    snapshot
}

pub(super) fn accepted_model_for(
    model: &ApplicationModel,
    settings: &GuiSettingsState,
    workflow: FeatureId,
) -> ModelUiState {
    let effective = effective_model_selection(settings, workflow).unwrap();
    let mut snapshot = model.model_snapshot.clone().unwrap();
    snapshot.generation += 1;
    snapshot.active = false;
    snapshot.selection.key.workflow = effective.workflow;
    snapshot.selection.key.source = effective.source;
    snapshot.selection.key.input = effective.input;
    let artifact = if effective.source == ModelSelectionSource::Canonical {
        crate::generated::RFDETR_PRESET_CATALOG
            .iter()
            .find(|preset| preset.presetname.as_ref() == effective.preset)
            .unwrap()
            .canonicalweightfilename
            .as_ref()
            .to_owned()
    } else {
        effective.artifact.into()
    };
    snapshot.selection.key.preset = effective.preset.into();
    snapshot.selection.key.resolution = effective.resolution;
    snapshot.selection.artifact = artifact;
    snapshot.terminal.outcome = ModelSelectionOutcome::Accepted;
    snapshot.terminal.detail.clear();
    snapshot
}
