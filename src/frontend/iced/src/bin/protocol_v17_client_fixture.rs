use std::collections::BTreeSet;
use std::fmt::Write as _;
use std::fs;
use std::io;
use std::path::PathBuf;

fn require(condition: bool, detail: &'static str) -> io::Result<()> {
    condition
        .then_some(())
        .ok_or_else(|| io::Error::other(detail))
}

fn validate_generated_surfaces() -> io::Result<()> {
    let request_ids: BTreeSet<_> = mmltk_browser_app::generated::APPLICATION_REQUEST_FIELDS
        .iter()
        .map(|field| field.field_id)
        .collect();
    require(
        !request_ids.is_empty()
            && request_ids.len() == mmltk_browser_app::generated::APPLICATION_REQUEST_FIELDS.len()
            && !request_ids.contains(&0)
            && mmltk_browser_app::generated::APPLICATION_REQUEST_FIELDS
                .iter()
                .all(|field| field.endpoint_id != 0),
        "generated request-field identities are invalid",
    )?;
    let settings_ids: BTreeSet<_> = mmltk_browser_app::generated::SETTINGS_LEAVES
        .iter()
        .map(|field| field.stable_field_id)
        .collect();
    require(
        !settings_ids.is_empty()
            && settings_ids.len() == mmltk_browser_app::generated::SETTINGS_LEAVES.len()
            && !settings_ids.contains(&0)
            && mmltk_browser_app::generated::SETTINGS_LEAVES
                .iter()
                .all(|leaf| {
                    !leaf.path.is_empty()
                        && (!leaf.finite
                            || leaf
                                .minimum
                                .into_iter()
                                .chain(leaf.maximum)
                                .all(f64::is_finite))
                        && leaf
                            .minimum
                            .zip(leaf.maximum)
                            .is_none_or(|(minimum, maximum)| minimum <= maximum)
                        && (leaf.maximum_bytes == 0 || leaf.minimum_bytes <= leaf.maximum_bytes)
                        && mmltk_browser_app::generated::REFLECTED_FIELD_FACTS
                            .iter()
                            .any(|field| {
                                leaf.owner == field.declaration_owner
                                    && leaf.member == field.name
                                    && leaf.catalog_provider == field.catalog_provider
                                    && leaf.finite == field.finite
                                    && leaf.minimum == field.minimum
                                    && leaf.maximum == field.maximum
                                    && leaf.minimum_bytes == field.min_bytes
                                    && leaf.maximum_bytes == field.max_bytes
                                    && leaf.maximum_items == field.max_items
                            })
                }),
        "generated settings identities are invalid",
    )?;
    let update = mmltk_browser_app::generated::update_currentview(
        mmltk_browser_app::generated::default_currentview().map_err(io::Error::other)?,
    );
    require(
        mmltk_browser_app::generated::SETTINGS_LEAVES
            .iter()
            .any(|leaf| leaf.mutable_leaf && leaf.path == update.path),
        "generated settings helper does not name a canonical mutable leaf",
    )?;
    for dialog in mmltk_browser_app::generated::FILE_DIALOGS {
        let leaf = mmltk_browser_app::generated::SETTINGS_LEAVES
            .iter()
            .find(|leaf| leaf.stable_field_id == dialog.stable_field_id);
        require(
            leaf.is_some_and(|leaf| {
                leaf.has_file_dialog
                    && leaf.path == dialog.field_path
                    && leaf.workflows == dialog.workflows
                    && !dialog.title.is_empty()
                    && !dialog.filter.is_empty()
                    && !dialog.pattern.is_empty()
            }),
            "generated file-dialog projection does not match its settings leaf",
        )?;
    }
    require(
        !mmltk_browser_app::generated::FILE_DIALOGS.is_empty(),
        "generated file-dialog projection is empty",
    )?;
    use mmltk_browser_app::generated as schema;
    let dialog_ids: BTreeSet<_> = schema::FILE_DIALOGS
        .iter()
        .map(|dialog| dialog.stable_field_id)
        .collect();
    require(
        dialog_ids.len() == schema::FILE_DIALOGS.len()
            && dialog_ids
                == schema::SETTINGS_LEAVES
                    .iter()
                    .filter(|leaf| leaf.has_file_dialog)
                    .map(|leaf| leaf.stable_field_id)
                    .collect(),
        "file-dialog projection is incomplete",
    )?;
    let artifact_ids: BTreeSet<_> = schema::MODEL_ARTIFACT_DIALOGS
        .iter()
        .map(|dialog| (dialog.stable_field_id, dialog.field_path))
        .collect();
    require(
        !artifact_ids.is_empty() && artifact_ids.len() == schema::MODEL_ARTIFACT_DIALOGS.len(),
        "model-artifact identities are duplicated",
    )?;
    for dialog in schema::MODEL_ARTIFACT_DIALOGS {
        require(
            dialog.target.stableid == dialog.stable_field_id
                && !dialog.dialog.title.is_empty()
                && !dialog.dialog.filter.is_empty()
                && !dialog.dialog.pattern.is_empty()
                && schema::SETTINGS_LEAVES.iter().any(|leaf| {
                    leaf.stable_field_id == dialog.stable_field_id && leaf.path == dialog.field_path
                })
                && dialog.key_fields.all()
                .into_iter()
                .chain(dialog.predicate_field_id)
                .all(|id| settings_ids.contains(&id))
                && schema::MODEL_SELECTION_COMPATIBILITY_CATALOG
                    .iter()
                    .any(|row| {
                        row.workflow == dialog.target.workflow
                            && row.input == dialog.target.input
                            && row.artifactfieldpath == dialog.field_path
                    }),
            "model-artifact relation does not identify canonical settings",
        )?;
    }
    require(
        schema::MODEL_SELECTION_COMPATIBILITY_CATALOG
            .iter()
            .all(|row| {
                schema::MODEL_ARTIFACT_DIALOGS
                    .iter()
                    .filter(|dialog| {
                        row.workflow == dialog.target.workflow
                            && row.input == dialog.target.input
                            && row.artifactfieldpath == dialog.field_path
                    })
                    .count()
                    == 1
            }),
        "model-artifact dialog projection is incomplete",
    )?;
    let provider_ids: BTreeSet<_> = mmltk_browser_app::generated::CATALOG_PROVIDERS
        .iter()
        .map(|provider| provider.stable_id)
        .collect();
    require(
        !provider_ids.is_empty()
            && provider_ids.len() == mmltk_browser_app::generated::CATALOG_PROVIDERS.len()
            && mmltk_browser_app::generated::CATALOG_PROVIDERS
                .iter()
                .all(|provider| {
                    provider.stable_id != 0
                        && !provider.identity.is_empty()
                        && !provider.row_type.is_empty()
                        && provider.row_count
                            == mmltk_browser_app::generated::CATALOG_ROWS
                                .iter()
                                .filter(|row| row.provider_id == provider.stable_id)
                                .count()
                }),
        "generated catalog projection is invalid",
    )?;
    let row_ids: BTreeSet<_> = mmltk_browser_app::generated::CATALOG_ROWS
        .iter()
        .map(|row| row.stable_id)
        .collect();
    require(
        !row_ids.is_empty()
            && row_ids.len() == mmltk_browser_app::generated::CATALOG_ROWS.len()
            && mmltk_browser_app::generated::CATALOG_ROWS
                .iter()
                .all(|row| row.stable_id != 0 && !row.key.is_empty()),
        "generated catalog row identities are invalid",
    )?;
    let typed_rows = mmltk_browser_app::generated::application_catalog_rows();
    require(
        typed_rows.len() == mmltk_browser_app::generated::CATALOG_ROWS.len()
            && typed_rows
                .iter()
                .map(|row| row.stable_id)
                .collect::<BTreeSet<_>>()
                == row_ids,
        "generated typed catalog rows are incomplete",
    )?;
    for row in typed_rows {
        use mmltk_browser_app::application_codec::IntoApplicationValue as _;
        let decoded = mmltk_browser_app::generated::decode_application_catalog_row(
            row.provider_id,
            row.value.clone().into_application_value(),
        )
        .map_err(io::Error::other)?;
        require(
            decoded == row.value
                && mmltk_browser_app::generated::CATALOG_ROWS
                    .iter()
                    .any(|fact| {
                        fact.provider_id == row.provider_id
                            && fact.stable_id == row.stable_id
                            && fact.key == row.key
                    }),
            "generated typed catalog row failed codec projection",
        )?;
    }
    let defaults =
        mmltk_browser_app::generated::application_settings_defaults().map_err(io::Error::other)?;
    require(
        defaults.len() == mmltk_browser_app::generated::SETTINGS_LEAVES.len()
            && defaults
                .iter()
                .map(|default| default.stable_field_id)
                .collect::<BTreeSet<_>>()
                == settings_ids
            && defaults.iter().all(|default| {
                mmltk_browser_app::generated::SETTINGS_LEAVES
                    .iter()
                    .any(|leaf| {
                        leaf.stable_field_id == default.stable_field_id && leaf.path == default.path
                    })
            }),
        "generated typed settings defaults are incomplete",
    )?;
    require(
        !mmltk_browser_app::generated::TRAIN_RECIPE_CATALOG_RELATION.is_empty()
            && mmltk_browser_app::generated::TRAIN_RECIPE_CATALOG_RELATION
                .iter()
                .all(|relation| {
                    relation.stable_field_id != 0
                        && !relation.source_path.is_empty()
                        && relation
                            .destination_path
                            .starts_with("workflows.train.request.")
                        && mmltk_browser_app::generated::SETTINGS_LEAVES
                            .iter()
                            .any(|leaf| {
                                leaf.stable_field_id == relation.stable_field_id
                                    && leaf.path == relation.destination_path
                            })
                })
            && mmltk_browser_app::generated::SETTINGS_LEAVES
                .iter()
                .all(|leaf| !leaf.path.contains("recipe_overrides")),
        "generated Train relation or opaque settings containment is invalid",
    )?;
    let recipe_provider = schema::CATALOG_PROVIDERS
        .iter()
        .find(|provider| provider.name == "TrainRecipeCatalog")
        .ok_or_else(|| io::Error::other("missing recipe catalog"))?;
    let selectors: BTreeSet<_> = schema::SETTINGS_LEAVES
        .iter()
        .filter(|leaf| leaf.catalog_provider == Some(recipe_provider.name))
        .map(|leaf| leaf.member)
        .collect();
    let recipe_members: BTreeSet<_> = schema::REFLECTED_FIELD_FACTS
        .iter()
        .filter(|field| field.owner == recipe_provider.row_type && !selectors.contains(field.name))
        .map(|field| field.name)
        .collect();
    let relation_sources: BTreeSet<_> = schema::TRAIN_RECIPE_CATALOG_RELATION
        .iter()
        .map(|relation| relation.source_path)
        .collect();
    let relation_destinations: BTreeSet<_> = schema::TRAIN_RECIPE_CATALOG_RELATION
        .iter()
        .map(|relation| relation.stable_field_id)
        .collect();
    require(
        !selectors.is_empty()
            && !recipe_members.is_empty()
            && relation_sources == recipe_members
            && relation_sources.len() == schema::TRAIN_RECIPE_CATALOG_RELATION.len()
            && relation_destinations.len() == relation_sources.len(),
        "recipe relation does not cover its canonical row members",
    )?;
    let snapshot_defaults =
        mmltk_browser_app::generated::application_snapshot_defaults().map_err(io::Error::other)?;
    let snapshots: Vec<_> = snapshot_defaults
        .iter()
        .map(|default| default.value.clone())
        .collect();
    require(
        mmltk_browser_app::generated::application_bootstrap_complete(&snapshots),
        "generated application bootstrap defaults are incomplete",
    )?;
    let mut missing = snapshots.clone();
    missing.pop();
    require(
        !mmltk_browser_app::generated::application_bootstrap_complete(&missing),
        "generated bootstrap completeness accepted a missing snapshot",
    )?;
    require(
        snapshots.len() >= 2,
        "bootstrap fixture needs distinct snapshot owners",
    )?;
    let mut duplicate = snapshots;
    duplicate[0] = duplicate[1].clone();
    require(
        !mmltk_browser_app::generated::application_bootstrap_complete(&duplicate),
        "generated bootstrap completeness accepted a duplicate snapshot",
    )?;
    let request_defaults =
        mmltk_browser_app::generated::application_request_defaults().map_err(io::Error::other)?;
    require(
        request_defaults.len() == mmltk_browser_app::generated::APPLICATION_REQUEST_FIELDS.len()
            && request_defaults
                .iter()
                .map(|default| default.field_id)
                .collect::<BTreeSet<_>>()
                == request_ids
            && request_defaults.iter().all(|default| {
                mmltk_browser_app::generated::APPLICATION_REQUEST_FIELDS
                    .iter()
                    .any(|field| {
                        field.endpoint_id == default.endpoint_id
                            && field.field_id == default.field_id
                    })
            }),
        "generated typed request defaults are incomplete",
    )?;
    let snapshot_ids: BTreeSet<_> = snapshot_defaults
        .iter()
        .map(|default| default.system_id)
        .collect();
    require(
        !snapshot_ids.is_empty()
            && snapshot_ids.len() == snapshot_defaults.len()
            && !snapshot_ids.contains(&0),
        "generated typed snapshot defaults are invalid",
    )?;
    require(
        mmltk_browser_app::generated::REFLECTED_FIELD_FACTS
            .iter()
            .any(|field| field.catalog_provider == Some("RfdetrPresetCatalog"))
            && mmltk_browser_app::generated::REFLECTED_FIELD_FACTS
                .iter()
                .any(|field| field.progress.is_some()),
        "generated reflected metadata projection is incomplete",
    )
}

fn validate_server_fixture() -> Result<(), Box<dyn std::error::Error>> {
    use mmltk_browser_app::generated::{ApplicationEvent, ApplicationReply, ApplicationSnapshot};
    use mmltk_browser_app::protocol::ServerRecord;

    let bytes = fs::read(env!("MMLTK_PROTOCOL_V17_SERVER_FIXTURE_PATH"))?;
    let mut records = Vec::new();
    let mut kinds = BTreeSet::new();
    let mut cursor = 0_usize;
    while cursor < bytes.len() {
        require(cursor + 4 <= bytes.len(), "truncated native fixture header")?;
        let size = u32::from_be_bytes(bytes[cursor..cursor + 4].try_into()?) as usize;
        cursor += 4;
        require(
            cursor + size <= bytes.len(),
            "truncated native fixture record",
        )?;
        let envelope = mmltk_browser_app::protocol::decode_envelope(&bytes[cursor..cursor + size])?;
        let kind = generated::ServerRecordKind::parse(envelope.kind.as_bytes())
            .ok_or("native server discriminator missing")?;
        kinds.insert(kind.wire_name());
        records.push(mmltk_browser_app::protocol::decode_server(
            &bytes[cursor..cursor + size],
        )?);
        cursor += size;
    }
    require(records.len() >= 8, "native fixture record count changed")?;
    require(
        matches!(&records[7], ServerRecord::IntegrationControl(record)
            if record.receipt.kind == generated::IntegrationControlKind::Advance && record.receipt.sequence == 2),
        "native integration advance fixture changed",
    )?;
    require(
        kinds.len() == generated::ServerRecordKind::ALL.len()
            && generated::ServerRecordKind::ALL.iter().all(|kind| {
                kinds.contains(kind.wire_name())
                    && generated::ServerRecordKind::parse(kind.wire_name().as_bytes())
                        == Some(*kind)
            }),
        "generated discriminator does not cover the complete native fixture",
    )?;
    require(
        generated::ServerRecordKind::parse(b"UnknownServerRecord").is_none(),
        "unknown server discriminator accepted",
    )?;
    require(
        matches!(&records[4], ServerRecord::InteractionRejected(record)
        if record.endpointid == generated::ENDPOINT_Annotation_Input && record.error.detail == "fixture input unavailable"),
        "native input rejection fixture changed",
    )?;
    require(
        matches!(&records[5], ServerRecord::InteractionRejected(record)
        if record.endpointid == generated::ENDPOINT_Explore_UpdateViewport && record.error.detail == "fixture unavailable"),
        "native interaction rejection fixture changed",
    )?;
    require(
        matches!(&records[6], ServerRecord::InteractionRejected(record)
        if record.error.detail.len() == generated::ReflectedRecordPath::ApplicationErrorRecord.map_child(b"detail").max_leaf_bytes()),
        "native rejection bounds changed",
    )?;
    let Some(ServerRecord::Bootstrap(bootstrap)) = records
        .iter()
        .find(|record| matches!(record, ServerRecord::Bootstrap(_)))
    else {
        return Err(io::Error::other("native Bootstrap fixture is missing").into());
    };
    require(
        bootstrap.input_epoch == 1
            && bootstrap.schema_fingerprint == mmltk_browser_app::generated::SCHEMA_FINGERPRINT
            && mmltk_browser_app::generated::application_bootstrap_complete(&bootstrap.snapshots),
        "native Bootstrap did not decode into the generated snapshot",
    )?;
    let mut visual = mmltk_browser_app::generated::ApplicationVisualSnapshots {
        explore: None,
        annotation: None,
        predict: None,
        validation: None,
        live: None,
        upscale: None,
    };
    for snapshot in &bootstrap.snapshots {
        match snapshot {
            ApplicationSnapshot::Explore(value) => visual.explore = Some(value),
            ApplicationSnapshot::Annotation(value) => visual.annotation = Some(value),
            ApplicationSnapshot::Predict(value) => visual.predict = Some(value),
            ApplicationSnapshot::Validation(value) => visual.validation = Some(value),
            ApplicationSnapshot::Live(value) => visual.live = Some(value),
            ApplicationSnapshot::Upscale(value) => visual.upscale = Some(value),
            _ => {}
        }
    }
    let Some(ServerRecord::IntentReply(reference)) = records.iter().find(
        |record| matches!(record, ServerRecord::IntentReply(reply) if reply.correlation == 19),
    ) else {
        return Err(io::Error::other("native named Annotation reference is missing").into());
    };
    use mmltk_browser_app::application_codec::{
        FromApplicationValue as _, IntoApplicationValue as _,
    };
    let model_reference = |correlation| -> Result<_, io::Error> {
        let Some(ServerRecord::IntentReply(reply)) = records.iter().find(
            |record| matches!(record, ServerRecord::IntentReply(reply) if reply.correlation == correlation),
        ) else { return Err(io::Error::other("native model projection pair is missing")); };
        reply.result.clone().map_err(|_| io::Error::other("native model projection pair is an error"))
    };
    let mut model_correlation = 400;
    for dialog in generated::MODEL_ARTIFACT_DIALOGS {
        for mode in 0..4 {
            let settings = generated::GuiSettingsState::from_application_value(model_reference(model_correlation)?)
                .map_err(io::Error::other)?;
            let expected_projection = generated::ModelSettingsProjection::from_application_value(model_reference(model_correlation + 1)?)
                .map_err(io::Error::other)?;
            model_correlation += 2;
            let actual = generated::project_model_settings(&settings, dialog.target.workflow)
                .ok_or_else(|| io::Error::other("generated model draft projection is missing"))?;
            require(actual == expected_projection, "native and Rust model projections differ")?;
            require(actual.key.classlayoutpath == "/tmp/fixture-model.classes.json",
                    "descriptor disappeared from a workflow projection")?;
            require(actual.key.input == if mode == 2 { generated::ModelArtifactInputKind::None } else { dialog.target.input },
                    "draft input meaning changed")?;
        }
    }
    let expected = mmltk_browser_app::generated::AnnotationSnapshot::from_application_value(
        reference
            .result
            .clone()
            .map_err(|_| io::Error::other("native reference is an error"))?,
    )
    .map_err(io::Error::other)?;
    require(
        visual.annotation == Some(&expected),
        "positional Annotation transport changed a logical, displayed or inactive field",
    )?;
    let transported = expected.clone().into_application_transport_value();
    if let mmltk_browser_app::application_codec::Value::Array(fields) = &transported {
        for count in [fields.len() - 1, fields.len() + 1] {
            let mut invalid = fields.clone();
            invalid.resize(count, mmltk_browser_app::application_codec::Value::Null);
            require(
                mmltk_browser_app::generated::AnnotationSnapshot::from_application_transport_value(
                    mmltk_browser_app::application_codec::Value::Array(invalid),
                )
                .is_err(),
                "positional Annotation decoder accepted an incorrect field count",
            )?;
        }
    } else {
        return Err(io::Error::other("generated Annotation output is not positional").into());
    }
    require(
        mmltk_browser_app::generated::AnnotationSnapshot::from_application_transport_value(
            transported,
        )
        .map_err(io::Error::other)?
            == expected,
        "generated positional Annotation codec does not round trip",
    )?;
    require(
        mmltk_browser_app::generated::AnnotationSnapshot::from_application_transport_value(
            expected.into_application_value(),
        )
        .is_err(),
        "positional output decoder accepted named persistence data",
    )?;
    use mmltk_browser_app::generated;
    let training = generated::TrainingRecord::from_application_value(model_reference(700)?).map_err(io::Error::other)?;
    let training_transport = generated::TrainingRecord::from_application_transport_value(model_reference(701)?).map_err(io::Error::other)?;
    require(training == training_transport && training.evaluatedweights == generated::EvaluatedWeights::Ema
        && training.progress.scalars.total == Some(1.25) && training.progress.scalars.classification.is_none()
        && training.progress.val.as_ref().is_some_and(|value| value.bbox.ap == 0.625)
        && training.droppedbefore == 2 && training.sequence == 17,
        "training metric values, unavailability or selected weight provenance changed")?;
    let training_page = generated::TrainingHistoryPage::from_application_value(model_reference(702)?).map_err(io::Error::other)?;
    let transported_training_page = generated::TrainingHistoryPage::from_application_transport_value(model_reference(703)?).map_err(io::Error::other)?;
    require(training_page == transported_training_page && training_page.generation == 9
        && training_page.nextcursor == 123 && training_page.records == [training],
        "bounded training history did not survive both codecs")?;
    let history = generated::encode_training_History(1, generated::TrainingHistoryQuery { generation: 9, cursor: 123, count: 32 });
    require(history.record.endpoint_id == 6009522715651029925,
        "training history endpoint identity changed")?;
    let metric_page = generated::EvaluationDetailPage::from_application_value(model_reference(600)?).map_err(io::Error::other)?;
    let transported_page = generated::EvaluationDetailPage::from_application_transport_value(model_reference(601)?).map_err(io::Error::other)?;
    require(metric_page == transported_page && metric_page.rows.len() == 2 && metric_page.rows[0].precisioncurve[9][100] == 0.125,
        "native validation detail grid did not survive both codecs")?;
    require(metric_page.rows[0].category.is_none() && metric_page.rows[0].categoryname.is_none()
        && metric_page.rows[1].category == Some(5)
        && metric_page.rows[1].categoryname.as_ref().is_some_and(|name| name.value == "é".repeat(128)),
        "bounded evaluated names or aggregate identity were lost")?;
    let axes = &generated::EVALUATION_AXIS_CATALOG[0];
    require(axes.iou == <[f64; 10]>::from_application_value(model_reference(604)?).map_err(io::Error::other)?
        && axes.iou == <[f64; 10]>::from_application_transport_value(model_reference(605)?).map_err(io::Error::other)?
        && axes.recall == <[f64; 101]>::from_application_value(model_reference(606)?).map_err(io::Error::other)?
        && axes.recall == <[f64; 101]>::from_application_transport_value(model_reference(607)?).map_err(io::Error::other)?
        && axes.confidence == <[f64; 101]>::from_application_value(model_reference(608)?).map_err(io::Error::other)?
        && axes.confidence == <[f64; 101]>::from_application_transport_value(model_reference(609)?).map_err(io::Error::other)?
        && axes.iou.len() == metric_page.rows[0].precisioncurve.len()
        && axes.recall.len() == metric_page.rows[0].precisioncurve[0].len(),
        "static metric axes differ from native values or actual curve extents")?;
    for correlation in [610, 612] {
        require(generated::EvaluationDetailPage::from_application_value(model_reference(correlation)?).is_err()
            && generated::EvaluationDetailPage::from_application_transport_value(model_reference(correlation + 1)?).is_err(),
            "metric page codec accepted oversized rows or category names")?;
    }
    let query = generated::encode_validation_Details(1, generated::EvaluationDetailQuery { generation: 7, offset: 0, count: 4 });
    require(query.record.endpoint_id == 8227943998713357562
        && query.record.fields.iter().map(|field| field.field_id).collect::<Vec<_>>() ==
            [14315253701558637748, 17534743922720314413, 12467602364773788561],
        "validation detail endpoint or stable request field identities changed")?;
    require(generated::REFLECTED_FIELD_FACTS.iter().any(|field| field.owner == "EvaluationMetricDetail"
        && field.name == "precision_curve" && field.catalog_provider == Some("EvaluationAxisCatalog")),
        "curve declaration lost its canonical axis provider")?;
    let sample_image = generated::ValidationImageMetadata::from_application_value(model_reference(602)?).map_err(io::Error::other)?;
    let transported_image = generated::ValidationImageMetadata::from_application_transport_value(model_reference(603)?).map_err(io::Error::other)?;
    require(sample_image == transported_image && sample_image.samples[0].identity.datasetindex == 3 && sample_image.samples[0].labels[0].groundtruth
        && !sample_image.overlays.predictionboxes && sample_image.overlays.predictionmasks
        && !sample_image.overlays.groundtruthboxes && sample_image.overlays.groundtruthmasks,
        "native validation sample image metadata did not survive both codecs")?;
    let validation = visual.validation.ok_or_else(|| io::Error::other("validation bootstrap is missing"))?;
    require(validation.metrics.as_ref().is_some_and(|summary| summary.bbox.areaap == [Some(0.0), None, Some(1.0)] && summary.mask.is_none())
        && validation.sampleavailable[0] && validation.sampleidentities[0].generation == 7,
        "validation summary availability or sample identity was lost")?;
    for (kind, session) in [
        (generated::PresentationSourceKind::None, 0),
        (generated::PresentationSourceKind::Explore, 1),
        (generated::PresentationSourceKind::Annotation, 2),
        (generated::PresentationSourceKind::Predict, 3),
        (generated::PresentationSourceKind::Live, 4),
        (generated::PresentationSourceKind::Upscale, 5),
    ] {
        require(
            generated::presentation_source_session(kind) == session,
            "stable browser session changed",
        )?;
        let Some(observed) = visual.observe(kind) else {
            require(session == 0, "producer observation missing")?;
            continue;
        };
        require(
            observed.snapshotrevision == if kind == generated::PresentationSourceKind::Predict { 7 + session } else { u64::MAX - session }
                && observed.frame.revision == 7 + session,
            "native visual observation differs from Rust projection",
        )?;
        let clean = generated::visual_clean_content_identity(observed.frame);
        require(
            clean.revision == 43 && clean.content.x == 1 && clean.extent.width == 32,
            "native clean-content identity differs from Rust projection",
        )?;
        let mut semantic = observed.frame.clone();
        semantic.revision += 1;
        require(
            generated::visual_clean_content_identity(&semantic) == clean,
            "semantic-only change replaced clean identity",
        )?;
        semantic.cleanrevision = 0;
        require(
            generated::visual_clean_content_identity(&semantic).revision == semantic.revision,
            "zero clean revision fallback changed",
        )?;
        semantic = observed.frame.clone();
        semantic.extent.width += 1;
        require(
            generated::visual_clean_content_identity(&semantic) != clean,
            "geometry change retained clean identity",
        )?;
    }
    for (correlation, selected) in [(17, false), (18, true)] {
        let Some(ServerRecord::IntentReply(reply)) = records.iter().find(|record| {
            matches!(
                record,
                ServerRecord::IntentReply(reply)
                    if reply.correlation == correlation
            )
        }) else {
            return Err(io::Error::other("native IntentReply fixture is missing").into());
        };
        let typed_reply = mmltk_browser_app::generated::decode_application_reply(
            mmltk_browser_app::generated::ENDPOINT_FileDialog_Open,
            reply.result.clone().map_err(|_| {
                io::Error::other("native fixture unexpectedly contains an error reply")
            })?,
        )
        .map_err(|error| {
            io::Error::other(format!(
                "native reply {correlation} typed decode failed: {error}"
            ))
        })?;
        let ApplicationReply::FileDialogOpen(snapshot) = typed_reply else {
            return Err(io::Error::other(
                "native reply did not decode into the generated reply union",
            )
            .into());
        };
        let selection = snapshot
            .selection
            .ok_or_else(|| io::Error::other("native dialog reply omitted its result"))?;
        require(
            mmltk_browser_app::generated::FILE_DIALOGS
                .iter()
                .any(|dialog| {
                    matches!(
                        &selection.target,
                        mmltk_browser_app::generated::FileDialogTarget::SettingsFieldTarget(target)
                            if dialog.stable_field_id == target.stableid
                    )
                }),
            "native dialog reply does not reference a generated dialog",
        )?;
        match selection.result {
            mmltk_browser_app::generated::
                FileDialogCancelledOrFileDialogSelectedVariant::FileDialogSelected(value) => {
                    require(selected, "native dialog selected/cancelled fixture changed")?;
                    require(
                        value.path == "/tmp/protocol-v17-fixture",
                        "native dialog selected path changed",
                    )?;
                }
            mmltk_browser_app::generated::
                FileDialogCancelledOrFileDialogSelectedVariant::FileDialogCancelled(_) => {
                    require(!selected, "native dialog selected/cancelled fixture changed")?;
                }
        }
    }
    let Some(ServerRecord::SystemEvent(event)) = records
        .iter()
        .find(|record| matches!(record, ServerRecord::SystemEvent(_)))
    else {
        return Err(io::Error::other("native SystemEvent fixture is missing").into());
    };
    require(
        matches!(&event.event, ApplicationEvent::SettingsSettingsChanged(_)),
        "native event did not decode into the generated event union",
    )?;
    Ok(())
}

fn annotation_edit_fixtures() -> Result<
    Vec<(&'static str, mmltk_browser_app::generated::AnnotationEdit)>,
    Box<dyn std::error::Error>,
> {
    use mmltk_browser_app::generated;
    let color = generated::AnnotationColor {
        hue: 120.0,
        saturation: 0.5,
        value: 0.75,
    };
    let range = generated::AnnotationColorRange {
        center: color.clone(),
        minus: color.clone(),
        plus: color,
        sampling: true,
    };
    Ok(vec![
        (
            "Intent:annotation.Edit.0",
            generated::AnnotationEdit::AnnotationToolEdit(generated::AnnotationToolEdit {
                tool: generated::AnnotationTool::Select,
            }),
        ),
        (
            "Intent:annotation.Edit.1",
            generated::AnnotationEdit::AnnotationSetupEdit(generated::AnnotationSetupEdit {
                action: generated::AnnotationSetupAction::ReloadFrame,
            }),
        ),
        (
            "Intent:annotation.Edit.2",
            generated::AnnotationEdit::AnnotationHoldEdit(generated::AnnotationHoldEdit {
                enabled: true,
            }),
        ),
        (
            "Intent:annotation.Edit.3",
            generated::AnnotationEdit::AnnotationSidebarEdit(generated::AnnotationSidebarEdit {
                command: generated::AnnotationSidebarCommand::Assist,
            }),
        ),
        (
            "Intent:annotation.Edit.4",
            generated::AnnotationEdit::AnnotationObjectEdit(generated::AnnotationObjectEdit {
                object: 0,
            }),
        ),
        (
            "Intent:annotation.Edit.5",
            generated::AnnotationEdit::AnnotationCategoryEdit(generated::AnnotationCategoryEdit {
                category: generated::AnnotationText::try_from("fixture")?,
            }),
        ),
        (
            "Intent:annotation.Edit.6",
            generated::AnnotationEdit::AnnotationSelectedObjectEdit(
                generated::AnnotationSelectedObjectEdit {
                    category: 0,
                    enabled: true,
                },
            ),
        ),
        (
            "Intent:annotation.Edit.7",
            generated::AnnotationEdit::AnnotationSplineEdit(generated::AnnotationSplineEdit {
                segment: 0,
            }),
        ),
        (
            "Intent:annotation.Edit.8",
            generated::AnnotationEdit::AnnotationSplineHandleEdit(
                generated::AnnotationSplineHandleEdit {
                    handle: generated::AnnotationHandleRole::SplineInHandle,
                    mode: generated::AnnotationSplineHandleMode::Corner,
                    point: generated::AnnotationPoint { x: 1.0, y: 2.0 },
                },
            ),
        ),
        (
            "Intent:annotation.Edit.9",
            generated::AnnotationEdit::AnnotationSkeletonEdit(generated::AnnotationSkeletonEdit {
                joint: 0,
            }),
        ),
        (
            "Intent:annotation.Edit.10",
            generated::AnnotationEdit::AnnotationMaskCleanupEdit(
                generated::AnnotationMaskCleanupEdit {
                    operation: generated::AnnotationMaskCleanup::LargestComponent,
                    radius: generated::default_uimaskcleanupradius().unwrap() as u16,
                },
            ),
        ),
        (
            "Intent:annotation.Edit.11",
            generated::AnnotationEdit::AnnotationMaskColorsEdit(
                generated::AnnotationMaskColorsEdit {
                    sup: range.clone(),
                    nosup: range,
                },
            ),
        ),
        (
            "Intent:annotation.Edit.12",
            generated::AnnotationEdit::AnnotationSceneEdit(generated::AnnotationSceneEdit {}),
        ),
        (
            "Intent:annotation.Edit.13",
            generated::AnnotationEdit::AnnotationUndoEdit(generated::AnnotationUndoEdit {}),
        ),
        (
            "Intent:annotation.Edit.14",
            generated::AnnotationEdit::AnnotationRedoEdit(generated::AnnotationRedoEdit {}),
        ),
        (
            "Intent:annotation.Edit.15",
            generated::AnnotationEdit::AnnotationClassEdit(generated::AnnotationClassEdit {
                category: 0,
            }),
        ),
    ])
}

fn application_record_fixtures() -> Result<Vec<(&'static str, Vec<u8>)>, Box<dyn std::error::Error>>
{
    use mmltk_browser_app::generated;

    let dialog = generated::FILE_DIALOGS
        .first()
        .ok_or_else(|| io::Error::other("fixture requires a reflected file dialog"))?;
    let model_dialog = generated::MODEL_ARTIFACT_DIALOGS
        .first()
        .ok_or_else(|| io::Error::other("fixture requires a reflected model artifact dialog"))?;
    let settings_update =
        generated::update_currentview(generated::default_currentview().map_err(io::Error::other)?);
    let mouse = generated::WorkspaceMouse {
        source: generated::PresentationSourceKind::Annotation,
        peerepoch: 1,
        documentepoch: 1,
        kind: generated::WorkspaceMouseKind::Motion,
        point: Some(generated::WorkspacePoint { x: 1.25, y: 2.5 }),
        wheelunit: generated::WorkspaceWheelUnit::Pixels,
        wheel: generated::WorkspacePoint { x: 0.125, y: -0.25 },
        ..generated::default_workspace_mouse()
    };
    let batch_encoded = generated::encode_workspace_mouse(mouse)?.encode()?;
    Ok(vec![
        (
            "Intent:settings.Update",
            generated::encode_settings_Update(
                19,
                generated::SettingsUpdateRequest {
                    updates: vec![settings_update],
                },
            )
            .record
            .encode()?,
        ),
        (
            "Intent:file_dialog.Open",
            generated::encode_filedialog_Open(
                17,
                generated::FileDialogOpen {
                    target: generated::FileDialogTarget::SettingsFieldTarget(
                        generated::SettingsFieldTarget {
                            stableid: dialog.stable_field_id,
                        },
                    ),
                },
            )
            .record
            .encode()?,
        ),
        (
            "Intent:file_dialog.Open.model_artifact",
            generated::encode_filedialog_Open(
                23,
                generated::FileDialogOpen {
                    target: generated::FileDialogTarget::ModelArtifactTarget(
                        model_dialog.target.clone(),
                    ),
                },
            )
            .record
            .encode()?,
        ),
        (
            "Interaction:explore.UpdateViewport",
            generated::encode_explore_UpdateViewport(generated::ExploreViewportUpdate {
                viewport: generated::ExploreViewport {
                    extent: generated::VisualExtent {
                        width: 64,
                        height: 64,
                    },
                    firstrow: 0,
                    rowcount: 1,
                    columns: 1,
                },
            })?
            .encode()?,
        ),
        ("Interaction:annotation.Input", batch_encoded),
    ])
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    use mmltk_browser_app::generated;

    let destination = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .ok_or("fixture destination is required")?;
    validate_generated_surfaces()?;
    validate_server_fixture()?;
    let mut output = String::new();
    let mut records = application_record_fixtures()?;
    for (index, (name, edit)) in annotation_edit_fixtures()?.into_iter().enumerate() {
        records.push((
            name,
            mmltk_browser_app::generated::encode_annotation_Edit(
                100 + index as u64,
                mmltk_browser_app::generated::AnnotationEditRequest { edit },
            )
            .record
            .encode()?,
        ));
    }
    records.push((
        "IntegrationControl",
        generated::IntegrationControl {
            protocolversion: generated::BROWSER_PROTOCOL_VERSION,
            receipt: generated::IntegrationControlReceipt {
                kind: generated::IntegrationControlKind::Settled,
                sequence: 1,
                progress: 0,
                failureline: 0,
                failure: String::new(),
                readgeneration: 0,
                compiledindex: 0,
            },
        }
        .encode()?,
    ));
    for (name, kind, generation, index) in [
        (
            "IntegrationControl:capacity",
            generated::IntegrationControlKind::CapacityArmRequested,
            0,
            0,
        ),
        (
            "IntegrationControl:visible-arm",
            generated::IntegrationControlKind::VisibleReadArmRequested,
            0,
            0,
        ),
        (
            "IntegrationControl:visible-release",
            generated::IntegrationControlKind::VisibleReadReleaseRequested,
            7,
            0,
        ),
    ] {
        records.push((
            name,
            generated::IntegrationControl {
                protocolversion: generated::BROWSER_PROTOCOL_VERSION,
                receipt: generated::IntegrationControlReceipt {
                    kind,
                    sequence: 1,
                    progress: 0,
                    failureline: 0,
                    failure: String::new(),
                    readgeneration: generation,
                    compiledindex: index,
                },
            }
            .encode()?,
        ));
    }
    for (kind, record) in records {
        write!(&mut output, "{kind} ")?;
        for byte in record {
            write!(&mut output, "{byte:02x}")?;
        }
        output.push('\n');
    }
    let temporary = destination.with_extension(format!("tmp.{}", std::process::id()));
    fs::write(&temporary, output)?;
    fs::rename(temporary, destination)?;
    Ok(())
}
