use super::model_selection::model_settings_projection;
use super::*;

pub(crate) fn physical_surface(
    frame: crate::presentation_surface::FrameReady,
) -> crate::presentation_surface::Surface {
    crate::presentation_surface::Surface {
        high: frame.high,
        low: frame.low,
        width: frame.content_width,
        height: frame.content_height,
        frame: Some(frame),
        crop: None,
        display_extent: None,
        viewer_identity: None,
        fit_revision: 0,
    }
}

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

pub(crate) fn gallery_layout(snapshot: &mut crate::generated::ExploreSnapshot) {
    let viewport = &snapshot.viewport;
    let side = viewport.extent.width / viewport.columns.max(1);
    snapshot.gallery.layout = crate::generated::ExploreAtlasLayout {
        firstrow: viewport.firstrow,
        rowcount: viewport.rowcount,
        rowcapacity: snapshot.frame.extent.height / side.max(1),
        roworigin: 0,
        columns: viewport.columns,
        cardextent: side,
    };
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
        resizemode: None,
        sourceextent: crate::generated::VisualExtent {
            width: 640,
            height: 480,
        },
        content: crate::generated::VisualRegion {
            x: 0,
            y: 0,
            width: 640,
            height: 480,
        },
    }
}

/// Coherent native Explore/control state and its exact physical publication.
/// Owners may vary visual labels or allocation identity after construction.
pub(crate) fn explore_presentation() -> (ApplicationModel, crate::presentation_surface::FrameReady)
{
    let mut model = bootstrapped();
    model.set_foreground_feature(FeatureId::Explore);
    let source = visual_frame(PresentationSourceKind::Explore, 1);
    let frame = physical_frame(1, 1, 5, source.extent.width, source.extent.height);
    let explore = model.explore.snapshot.as_mut().unwrap();
    explore.ready = true;
    explore.revision = 10;
    explore.mode = crate::generated::ExploreMode::Detail;
    explore.dataset.identity = 1;
    explore.selectedimage = Some(0);
    explore.frame = source.clone();
    let control = model.presentation.as_mut().unwrap();
    control.selected = source.source.clone();
    crate::presentation_surface::metadata::install_explore(frame, explore);
    (model, frame)
}

/// All DOM-notification/model arrival orders. Physical GPU copy ordering is
/// independent of when either notification is consumed by the presentation model.
pub(crate) fn presentation_arrival_orders() -> impl Iterator<Item = [usize; 4]> {
    (0..4).flat_map(|domain| {
        (0..4)
            .filter(move |control| *control != domain)
            .flat_map(move |control| {
                (0..4)
                    .filter(move |publication| *publication != domain && *publication != control)
                    .map(move |publication| {
                        let copied = (0..4)
                            .find(|position| {
                                *position != domain
                                    && *position != control
                                    && *position != publication
                            })
                            .expect("remaining copy notification position");
                        [domain, control, publication, copied]
                    })
            })
    })
}

pub(crate) fn physical_frame(
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
    content_width: u32,
    content_height: u32,
) -> crate::presentation_surface::FrameReady {
    crate::presentation_surface::FrameReady {
        source_high: 0,
        source_low: 0,
        direct_sampling: false,
        high: 1,
        low: 2,
        layer: 0,
        slot: 0,
        content_session,
        content_sequence,
        presentation_revision,
        content_width,
        content_height,
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

pub(crate) fn accepted_model_for(
    model: &ApplicationModel,
    settings: &GuiSettingsState,
    workflow: FeatureId,
) -> ModelUiState {
    let effective = model_settings_projection(settings, workflow).unwrap();
    let mut snapshot = model.model_snapshot.clone().unwrap();
    snapshot.generation += 1;
    snapshot.active = false;
    let artifact = if effective.selection.key.source == ModelSelectionSource::Canonical {
        crate::generated::RFDETR_PRESET_CATALOG
            .iter()
            .find(|preset| preset.presetname.as_ref() == effective.selection.key.preset)
            .unwrap()
            .canonicalweightfilename
            .as_ref()
            .to_owned()
    } else {
        effective.selection.artifact.into()
    };
    snapshot.selection.key = effective.selection.key;
    snapshot.selection.artifact = artifact;
    snapshot.terminal.outcome = ModelSelectionOutcome::Accepted;
    snapshot.terminal.detail.clear();
    snapshot
}

pub(crate) fn validation_image_metadata() -> crate::generated::ValidationImageMetadata {
    use crate::generated::*;
    let snapshot = bootstrapped().workflow.validation.unwrap();
    ValidationImageMetadata {
        frame: {
            let mut frame = visual_frame(PresentationSourceKind::Validation, 1);
            frame.extent.width = 512;
            frame.extent.height = 576;
            frame.content = VisualRegion {
                x: 0,
                y: 0,
                width: 512,
                height: 576,
            };
            frame.cleanrevision = 9;
            frame
        },
        contentidentity: 9,
        detail: false,
        selected: None,
        document: snapshot.document,
        overlays: snapshot.overlays,
        samples: std::array::from_fn(|index| ValidationSampleMetadata {
            identity: ValidationSampleIdentity {
                generation: 7,
                datasetindex: index as u32,
            },
            available: index < 2,
            crop: VisualRegion {
                x: index as u32 % 2 * 256 + 32,
                y: index as u32 / 2 * 192,
                width: 192,
                height: 192,
            },
            sourceextent: VisualExtent {
                width: 400,
                height: 200,
            },
            content: VisualRegion {
                x: 0,
                y: 0,
                width: 200,
                height: 200,
            },
            pixelextent: VisualExtent {
                width: 200,
                height: 200,
            },
            labels: if index < 2 {
                vec![ValidationLabel {
                    rgb: crate::application_codec::ByteArray([255, 0, 0]),
                    box_: AnnotationBox {
                        first: AnnotationPoint { x: 20.0, y: 40.0 },
                        second: AnnotationPoint { x: 80.0, y: 90.0 },
                    },
                    color: AnnotationColor {
                        hue: 120.0,
                        saturation: 1.0,
                        value: 0.8,
                    },
                    category: index as u32,
                    groundtruth: index == 0,
                    confidence: 0.75,
                    name: "paired name".into(),
                }]
            } else {
                Vec::new()
            },
        }),
    }
}

pub(crate) fn saved_training_run(
    configuration: crate::generated::TrainRequest,
) -> crate::generated::TrainingOpenedRun {
    use crate::generated::*;
    TrainingOpenedRun {
        generation: 3,
        directory: "saved-output".into(),
        run: Some(TrainingRun {
            formatversion: 2,
            runid: "saved".into(),
            attemptid: "saved-attempt".into(),
            checkpointattemptid: String::new(),
            sourcecheckpointattemptid: String::new(),
            configuration,
            execution: TrainingExecutionFacts {
                evallanes: 1,
                effectivebatchperrank: 4,
                effectivebatchglobal: 4,
                datasetlimits: TrainingDatasetLimits {
                    trainmaxinstances: 1,
                    valmaxinstances: 1,
                    testmaxinstances: None,
                    largestmaxinstances: 1,
                    resolvednumqueries: 6,
                    requirednumqueries: 1,
                    automaticnumqueriescap: 6,
                    querysource: "fixture".into(),
                    requestedoverride: false,
                    automatic: false,
                },
            },
            originalweights: "fixture.pt".into(),
            originalclassdescriptor: String::new(),
            evaluatedweights: EvaluatedWeights::Ordinary,
            classlayout: ModelClassLayout {
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
            },
            resumeepoch: -1,
            resumeoptimizerstep: 0,
        }),
    }
}
