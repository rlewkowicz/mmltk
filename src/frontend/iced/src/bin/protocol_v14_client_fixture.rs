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
            && !request_ids.contains(&0),
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
                    (!leaf.finite
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
                                leaf.path.rsplit('.').next() == Some(field.name)
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
            }),
            "generated file-dialog projection does not match its settings leaf",
        )?;
    }
    require(
        !mmltk_browser_app::generated::FILE_DIALOGS.is_empty(),
        "generated file-dialog projection is empty",
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
        typed_rows.len() == mmltk_browser_app::generated::CATALOG_ROWS.len(),
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
        mmltk_browser_app::generated::TRAIN_RECIPE_CATALOG_RELATION.len() == 11
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
    let snapshot_defaults =
        mmltk_browser_app::generated::application_snapshot_defaults().map_err(io::Error::other)?;
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

    let bytes = fs::read(env!("MMLTK_PROTOCOL_V14_SERVER_FIXTURE_PATH"))?;
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
        let kind = generated::ServerRecordKind::parse(envelope.kind.as_bytes()).ok_or("native server discriminator missing")?;
        kinds.insert(kind.wire_name());
        records.push(mmltk_browser_app::protocol::decode_server(
            &bytes[cursor..cursor + size],
        )?);
        cursor += size;
    }
    require(records.len() == 7, "native fixture record count changed")?;
    require(kinds.len() == generated::ServerRecordKind::ALL.len()
        && generated::ServerRecordKind::ALL.iter().all(|kind| kinds.contains(kind.wire_name())
            && generated::ServerRecordKind::parse(kind.wire_name().as_bytes()) == Some(*kind)),
        "generated discriminator does not cover the complete native fixture")?;
    require(generated::ServerRecordKind::parse(b"UnknownServerRecord").is_none(), "unknown server discriminator accepted")?;
    require(matches!(&records[4], ServerRecord::InputProgress(mmltk_browser_app::generated::InputProgress { progress, error: None , .. })
        if progress.epoch == 1 && progress.consumedsequence == 2 && progress.rejection.is_none()), "native cumulative progress fixture changed")?;
    require(matches!(&records[5], ServerRecord::InteractionRejected(record)
        if record.endpointid == generated::ENDPOINT_Explore_UpdateViewport && record.error.category == generated::ApplicationErrorCategory::Unavailable && record.error.detail == "fixture unavailable"), "native interaction rejection fixture changed")?;
    require(matches!(&records[6], ServerRecord::InputProgress(record)
        if record.progress.rejection.as_ref().is_some_and(|detail| detail.len() == generated::ReflectedRecordPath::AnnotationInputProgress.map_child(b"rejection").max_leaf_bytes())
            && record.error.as_ref().is_some_and(|error| error.category == generated::ApplicationErrorCategory::Busy && error.detail == "fixture busy")),
        "native optional rejection bounds changed")?;
    let Some(ServerRecord::Bootstrap(bootstrap)) = records
        .iter()
        .find(|record| matches!(record, ServerRecord::Bootstrap(_)))
    else {
        return Err(io::Error::other("native Bootstrap fixture is missing").into());
    };
    require(
        bootstrap.input_epoch == 1 && bootstrap.schema_fingerprint == mmltk_browser_app::generated::SCHEMA_FINGERPRINT
            && mmltk_browser_app::generated::application_bootstrap_complete(&bootstrap.snapshots),
        "native Bootstrap did not decode into the generated snapshot",
    )?;
    let mut visual = mmltk_browser_app::generated::ApplicationVisualSnapshots {
        explore: None,
        annotation: None,
        predict: None,
        live: None,
        upscale: None,
    };
    for snapshot in &bootstrap.snapshots {
        match snapshot {
            ApplicationSnapshot::Explore(value) => visual.explore = Some(value),
            ApplicationSnapshot::Annotation(value) => visual.annotation = Some(value),
            ApplicationSnapshot::Predict(value) => visual.predict = Some(value),
            ApplicationSnapshot::Live(value) => visual.live = Some(value),
            ApplicationSnapshot::Upscale(value) => visual.upscale = Some(value),
            _ => {}
        }
    }
    use mmltk_browser_app::generated;
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
            observed.snapshotrevision == u64::MAX - session
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
                        value.path == "/tmp/protocol-v14-fixture",
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
    let batch = generated::AnnotationInputBatch {
        epoch: 7, documentepoch: 1, sequence: 1,
        samples: (0..generated::ANNOTATION_INPUT_BATCH_CAPACITY).map(|index| generated::AnnotationPointer {
                phase: if index == 0 { generated::AnnotationPointerPhase::Begin }
                    else if index + 1 == generated::ANNOTATION_INPUT_BATCH_CAPACITY { generated::AnnotationPointerPhase::End }
                    else { generated::AnnotationPointerPhase::Update }, interactionid: 1, sequence: index as u64 + 1,
                target: generated::AnnotationPointerTarget { object: Some(1), element: Some(2), role: Some(generated::AnnotationHandleRole::BoxCorner) },
                point: generated::AnnotationPoint { x: index as f32, y: 1.0 },
                brushradius: generated::default_uiannotationbrushradius().unwrap() as u16,
            }).collect(),
    };
    let ordinary = generated::encode_annotation_Input(batch.clone())?.encode()?;
    let mut scratch = Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY);
    let mut batch_encoded = Vec::with_capacity(generated::ANNOTATION_INPUT_ENCODED_CAPACITY);
    let retained = (scratch.as_ptr(), scratch.capacity(), batch_encoded.as_ptr(), batch_encoded.capacity());
    for _ in 0..2 {
        generated::encode_annotation_Input_into(&batch, &mut scratch, &mut batch_encoded)?;
        require(batch_encoded == ordinary, "retained and owned compact encoders disagree")?;
        require(retained == (scratch.as_ptr(), scratch.capacity(), batch_encoded.as_ptr(), batch_encoded.capacity()),
            "32-sample encoding grew retained storage")?;
    }
    let mut oversized = batch;
    oversized.samples.push(oversized.samples[0].clone());
    require(generated::encode_annotation_Input(oversized).is_err(), "compact sample bound was not enforced")?;
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
                viewport: generated::default_request_exploreUpdateViewportviewport()
                    .map_err(io::Error::other)?,
                focusedcompiledindex: None,
            })?
            .encode()?,
        ),
        (
            "Interaction:annotation.Input",
            batch_encoded,
        ),
    ])
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
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
        "RendererObservation",
        mmltk_browser_app::protocol::RendererObservation::Surface {
            width: 640,
            height: 480,
            scale: 1.5,
        }
        .encode()?,
    ));
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
