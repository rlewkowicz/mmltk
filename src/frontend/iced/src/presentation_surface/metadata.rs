use super::{AnnotationContent, DetailContent, FrameReady, PendingImage, Placement, Surface};
use crate::application_codec::FromApplicationValue;
use crate::generated::{self, WorkspaceImageProduct};
use std::{cell::RefCell, sync::Arc};

// One immutable, physically paired product travels through pending and completed
// publication. Prepared caches share its generated native allocation.
#[derive(Clone, Default)]
pub(super) struct Content {
    product: Option<WorkspaceImageProduct>,
    prepared: Prepared,
}
#[derive(Clone, Default)]
enum Prepared {
    #[default]
    None,
    Gallery(Arc<super::labels::GalleryContent>),
    Detail(DetailContent),
    Prediction(Arc<super::labels::PredictionContent>),
    Validation(Arc<super::labels::ValidationContent>),
}
impl Content {
    pub(super) fn gallery(&self) -> Option<&Arc<super::labels::GalleryContent>> {
        match &self.prepared {
            Prepared::Gallery(value) => Some(value),
            _ => None,
        }
    }
    pub(super) fn detail(&self) -> Option<DetailContent> {
        match &self.prepared {
            Prepared::Detail(value) => Some(value.clone()),
            _ => None,
        }
    }
    pub(super) fn annotation(&self) -> Option<AnnotationContent> {
        match self.product.as_ref()? {
            WorkspaceImageProduct::Annotation(value) => Some(AnnotationContent {
                metadata: value.clone(),
            }),
            _ => None,
        }
    }
    pub(super) fn prediction(&self) -> Option<Arc<super::labels::PredictionContent>> {
        match &self.prepared {
            Prepared::Prediction(value) => Some(value.clone()),
            _ => None,
        }
    }
    pub(super) fn validation(&self) -> Option<Arc<super::labels::ValidationContent>> {
        match &self.prepared {
            Prepared::Validation(value) => Some(value.clone()),
            _ => None,
        }
    }
    pub(super) fn from_gallery(value: Option<Arc<super::labels::GalleryContent>>) -> Self {
        let Some(value) = value else {
            return Self::default();
        };
        Self {
            product: Some(WorkspaceImageProduct::Explore(value.metadata.clone())),
            prepared: Prepared::Gallery(value),
        }
    }
    #[cfg(test)]
    pub(super) fn from_detail(value: Option<DetailContent>) -> Self {
        let Some(value) = value else {
            return Self::default();
        };
        Self {
            product: Some(match &value.upscale {
                Some(upscale) => WorkspaceImageProduct::Upscale(upscale.clone()),
                None => WorkspaceImageProduct::Explore(value.explore.clone()),
            }),
            prepared: Prepared::Detail(value),
        }
    }
}

#[derive(Clone)]
struct Image {
    frame: FrameReady,
    product: generated::VisualFrame,
    pending: PendingImage,
    #[cfg(target_arch = "wasm32")]
    transfer: u64,
    #[cfg(target_arch = "wasm32")]
    bytes: usize,
    #[cfg(target_arch = "wasm32")]
    fingerprint: Option<u64>,
}

thread_local! {
    static IMAGES: RefCell<Vec<Image>> = RefCell::new(Vec::with_capacity(super::SAMPLE_CAPACITY));
}

pub(super) fn valid_content(frame: &generated::VisualFrame) -> bool {
    frame.source.instance != 0 && frame.revision != 0 && valid_region(&frame.content, &frame.extent)
}

fn valid_region(region: &generated::VisualRegion, extent: &generated::VisualExtent) -> bool {
    region.width != 0
        && region.height != 0
        && region
            .x
            .checked_add(region.width)
            .is_some_and(|end| end <= extent.width)
        && region
            .y
            .checked_add(region.height)
            .is_some_and(|end| end <= extent.height)
}

fn valid_detail(source: &generated::ExploreImageMetadata) -> bool {
    source.mode == generated::ExploreMode::Detail
        && source.dataset.identity != 0
        && source.selectedimage.is_some()
        && valid_content(&source.frame)
}

fn valid_validation(snapshot: &generated::ValidationImageMetadata) -> bool {
    snapshot.contentidentity != 0
        && valid_content(&snapshot.frame)
        && snapshot.detail == snapshot.selected.is_some()
        && (!snapshot.detail
            || snapshot.samples.iter().any(|sample| {
                sample.available
                    && Some(&sample.identity) == snapshot.selected.as_ref()
                    && snapshot.frame.extent == sample.pixelextent
                    && snapshot.frame.content == sample.content
                    && snapshot.frame.sourceextent == sample.sourceextent
            }))
        && snapshot.samples.iter().all(|sample| {
            !sample.available
                || (sample.identity.generation != 0
                    && sample.pixelextent.width != 0
                    && sample.pixelextent.height != 0
                    && valid_region(&sample.content, &sample.pixelextent)
                    && ((snapshot.detail && Some(&sample.identity) != snapshot.selected.as_ref())
                        || (sample.crop.width != 0
                            && sample.crop.height != 0
                            && sample
                                .crop
                                .x
                                .checked_add(sample.crop.width)
                                .is_some_and(|end| end <= snapshot.frame.extent.width)
                            && sample
                                .crop
                                .y
                                .checked_add(sample.crop.height)
                                .is_some_and(|end| end <= snapshot.frame.extent.height))))
        })
}

pub(crate) fn install(
    frame: FrameReady,
    width: u32,
    height: u32,
    transfer: u64,
    bytes: &[u8],
) -> Result<(), String> {
    if transfer == 0 || width < frame.content_width || height < frame.content_height {
        return Err("invalid graphics image capacity".into());
    }
    let value = crate::protocol::cbor::decode_graphics_value(
        bytes,
        generated::WORKSPACE_METADATA_BYTE_CAPACITY,
    )
    .map_err(|error| error.0)?;
    let metadata = generated::WorkspaceImageMetadata::from_application_transport_value(value)?;
    if metadata.schemafingerprint != generated::SCHEMA_FINGERPRINT
        || !frame.matches_content(&metadata.frame)
    {
        return Err("graphics image metadata identity mismatch".into());
    }
    let product = generated::decode_workspace_image_product(
        metadata.product.systemid,
        metadata.product.value,
    )?;
    let source = metadata
        .source
        .map(|source| generated::decode_workspace_image_product(source.systemid, source.value))
        .transpose()?;
    if source.is_some() && !matches!(&product, WorkspaceImageProduct::Upscale(_)) {
        return Err("unexpected graphics source metadata".into());
    }
    let prepared = match &product {
        WorkspaceImageProduct::Explore(snapshot) if snapshot.frame == metadata.frame => {
            if snapshot.mode == generated::ExploreMode::Gallery {
                if !super::gallery::valid_layout(snapshot) {
                    return Err("invalid graphics atlas metadata".into());
                }
            } else if !valid_detail(snapshot) {
                return Err("invalid graphics detail metadata".into());
            }
            if snapshot.mode == generated::ExploreMode::Gallery {
                Prepared::Gallery(Arc::new(super::labels::GalleryContent::new(
                    snapshot.clone(),
                )))
            } else {
                Prepared::Detail(DetailContent::new(snapshot.clone(), None))
            }
        }
        WorkspaceImageProduct::Annotation(snapshot) if snapshot.frame == metadata.frame => {
            Prepared::None
        }
        WorkspaceImageProduct::Upscale(snapshot) if snapshot.frame == metadata.frame => {
            if !valid_content(&snapshot.frame)
                || snapshot.frame.sourceextent != snapshot.input.sourceextent
                || !valid_content(&snapshot.input)
                || snapshot.preparedextent.width < snapshot.input.extent.width
                || snapshot.preparedextent.height < snapshot.input.extent.height
                || !valid_region(&snapshot.preparedcontent, &snapshot.preparedextent)
                || snapshot
                    .preparedextent
                    .checked_scale(generated::UpscaleImageMetadata::OUTPUT_SCALE)
                    .as_ref()
                    != Some(&snapshot.frame.extent)
                || snapshot
                    .preparedcontent
                    .checked_scale(generated::UpscaleImageMetadata::OUTPUT_SCALE)
                    .as_ref()
                    != Some(&snapshot.frame.content)
            {
                return Err("invalid derived image geometry".into());
            }
            match &source {
                Some(WorkspaceImageProduct::Explore(source))
                    if source.frame == snapshot.input && valid_detail(source) =>
                {
                    Prepared::Detail(DetailContent::new(source.clone(), Some(snapshot.clone())))
                }
                Some(WorkspaceImageProduct::Validation(source))
                    if source.frame == snapshot.input
                        && source.detail
                        && valid_validation(source) =>
                {
                    Prepared::Validation(Arc::new(
                        super::labels::ValidationContent::new(source.clone())
                            .with_upscale(snapshot.clone()),
                    ))
                }
                _ => return Err("graphics detail source identity mismatch".into()),
            }
        }
        WorkspaceImageProduct::Predict(snapshot)
            if snapshot.frame == metadata.frame && snapshot.contentidentity != 0 =>
        {
            Prepared::Prediction(Arc::new(super::labels::PredictionContent::new(
                snapshot.clone(),
            )))
        }
        WorkspaceImageProduct::Validation(snapshot)
            if snapshot.frame == metadata.frame && snapshot.contentidentity != 0 =>
        {
            if !valid_validation(snapshot) {
                return Err("invalid validation sample image geometry".into());
            }
            Prepared::Validation(Arc::new(super::labels::ValidationContent::new(
                snapshot.clone(),
            )))
        }
        WorkspaceImageProduct::Live(snapshot) if snapshot.frame == metadata.frame => Prepared::None,
        _ => return Err("graphics metadata does not describe this visual product".into()),
    };
    let content = Content {
        product: Some(product),
        prepared,
    };
    let surface = Surface {
        high: frame.high,
        low: frame.low,
        width,
        height,
        frame: Some(frame),
        crop: None,
        display_extent: None,
        viewer_identity: content
            .prediction()
            .as_ref()
            .map(|content| (frame.content_session, content.metadata.contentidentity))
            .or_else(|| {
                content.validation().as_ref().map(|content| {
                    (
                        generated::presentation_source_session(
                            generated::PresentationSourceKind::Validation,
                        ),
                        content.metadata.contentidentity,
                    )
                })
            })
            .or_else(|| {
                content
                    .detail()?
                    .viewer_identity()
                    .map(|(dataset, image)| (dataset, u64::from(image)))
            }),
        fit_revision: 0,
    };
    let placement = content.gallery().map_or(Placement::Contain, |snapshot| {
        super::gallery::placement(&snapshot.metadata)
    });
    let image = Image {
        frame,
        product: metadata.frame,
        pending: PendingImage {
            read: None,
            surface,
            content,
            placement,
            complete: super::copy_completed(frame),
            view_ready: true,
        },
        #[cfg(target_arch = "wasm32")]
        transfer,
        #[cfg(target_arch = "wasm32")]
        bytes: bytes.len(),
        #[cfg(target_arch = "wasm32")]
        fingerprint: super::surface_trace_enabled().then(|| fingerprint(bytes)),
    };
    IMAGES.with(|images| {
        let mut images = images.borrow_mut();
        if images.iter().any(|image| image.frame == frame) {
            return Err("duplicate graphics image metadata".into());
        }
        if images.len() == super::SAMPLE_CAPACITY {
            return Err("graphics metadata custody capacity exhausted".into());
        }
        if let Some(gallery) = image.pending.content.gallery().map(Arc::as_ref) {
            super::trace_gallery_source(&gallery.metadata);
        }
        images.push(image);
        Ok(())
    })
}

#[cfg(target_arch = "wasm32")]
fn fingerprint(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf29ce484222325_u64, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3)
    })
}

pub(crate) fn surface(frame: FrameReady) -> Option<Surface> {
    IMAGES.with(|images| {
        images
            .borrow()
            .iter()
            .find(|image| image.frame == frame)
            .map(|image| image.pending.surface)
    })
}

pub(super) fn pending(frame: FrameReady) -> Option<PendingImage> {
    IMAGES.with(|images| {
        images
            .borrow()
            .iter()
            .find(|image| image.frame == frame)
            .map(|image| image.pending.clone())
    })
}

pub(crate) fn product(frame: FrameReady) -> Option<generated::VisualFrame> {
    IMAGES.with(|images| {
        images
            .borrow()
            .iter()
            .find(|image| image.frame == frame)
            .map(|image| image.product.clone())
    })
}

#[cfg(target_arch = "wasm32")]
pub(super) fn trace_fields(frame: FrameReady) -> String {
    IMAGES.with(|images| images.borrow().iter().find(|image| image.frame == frame)
        .and_then(|image| image.fingerprint.map(|fingerprint|
            format!(",\"transfer_sequence\":{},\"metadata_bytes\":{},\"metadata_fingerprint\":\"{fingerprint:016x}\"",
                image.transfer, image.bytes))).unwrap_or_default())
}

pub(crate) fn retire(frame: FrameReady) {
    IMAGES.with(|images| images.borrow_mut().retain(|image| image.frame != frame));
}

#[cfg(test)]
pub(crate) fn reset() {
    IMAGES.with(|images| images.borrow_mut().clear());
}

#[cfg(test)]
pub(crate) fn encode_product<T: crate::application_codec::IntoApplicationValue>(
    system: generated::ApplicationSystem,
    product: T,
) -> generated::SystemSnapshot {
    generated::SystemSnapshot {
        systemid: generated::application_system_stable_id(system),
        value: product.into_application_transport_value(),
    }
}

#[cfg(test)]
pub(crate) fn encode(
    frame: generated::VisualFrame,
    product: generated::SystemSnapshot,
    source: Option<generated::SystemSnapshot>,
) -> Vec<u8> {
    use crate::application_codec::IntoApplicationValue;
    let metadata = generated::WorkspaceImageMetadata {
        schemafingerprint: generated::SCHEMA_FINGERPRINT,
        frame,
        product,
        source,
    };
    let mut bytes = Vec::new();
    crate::protocol::cbor::encode_value(&metadata.into_application_transport_value(), &mut bytes)
        .unwrap();
    bytes
}

#[cfg(test)]
pub(crate) fn install_explore(frame: FrameReady, snapshot: &generated::ExploreSnapshot) {
    retire(frame);
    install(
        frame,
        frame.content_width,
        frame.content_height,
        frame.presentation_revision,
        &encode(
            snapshot.frame.clone(),
            encode_product(
                generated::ApplicationSystem::Explore,
                generated::ExploreImageMetadata::from(snapshot),
            ),
            None,
        ),
    )
    .unwrap();
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn generated_pixel_geometry_scaling_checks_each_member() {
        let scale = generated::UpscaleImageMetadata::OUTPUT_SCALE;
        assert_eq!(scale, 4);
        let region = generated::VisualRegion {
            x: 2,
            y: 3,
            width: 5,
            height: 7,
        };
        assert_eq!(
            region.checked_scale(scale),
            Some(generated::VisualRegion {
                x: 8,
                y: 12,
                width: 20,
                height: 28,
            })
        );
        assert_eq!(
            region.checked_scale(0),
            Some(generated::VisualRegion {
                x: 0,
                y: 0,
                width: 0,
                height: 0
            })
        );
        let limit = u32::MAX / scale;
        assert_eq!(
            generated::VisualExtent {
                width: limit,
                height: limit
            }
            .checked_scale(scale),
            Some(generated::VisualExtent {
                width: limit * scale,
                height: limit * scale
            })
        );
        for member in 0..6 {
            let mut extent = generated::VisualExtent {
                width: 1,
                height: 1,
            };
            let mut region = generated::VisualRegion {
                x: 1,
                y: 1,
                width: 1,
                height: 1,
            };
            match member {
                0 => extent.width = limit + 1,
                1 => extent.height = limit + 1,
                2 => region.x = limit + 1,
                3 => region.y = limit + 1,
                4 => region.width = limit + 1,
                _ => region.height = limit + 1,
            }
            assert_eq!(extent.checked_scale(scale).is_none(), member < 2);
            assert_eq!(region.checked_scale(scale).is_none(), member >= 2);
        }
    }

    #[test]
    fn paired_geometry_rejects_missing_source_and_inconsistent_samples() {
        let (model, _) = crate::view_model::test_support::explore_presentation();
        let mut detail =
            generated::ExploreImageMetadata::from(model.explore.snapshot.as_ref().unwrap());
        for region in [
            generated::VisualRegion {
                x: 0,
                y: 0,
                width: 640,
                height: 480,
            },
            generated::VisualRegion {
                x: 0,
                y: 80,
                width: 640,
                height: 320,
            },
        ] {
            detail.frame.content = region;
            assert!(valid_detail(&detail));
            for (width, height) in [(0, 480), (640, 0)] {
                let mut invalid = detail.clone();
                invalid.frame.sourceextent = generated::VisualExtent { width, height };
                assert!(!valid_detail(&invalid));
            }
        }
        let mut image = crate::view_model::test_support::validation_image_metadata();
        for region in [
            generated::VisualRegion {
                x: 0,
                y: 0,
                width: 200,
                height: 200,
            },
            generated::VisualRegion {
                x: 0,
                y: 50,
                width: 200,
                height: 100,
            },
        ] {
            image.detail = true;
            image.selected = Some(image.samples[0].identity.clone());
            image.samples[0].content = region.clone();
            image.frame.content = region;
            image.frame.extent = image.samples[0].pixelextent.clone();
            image.frame.sourceextent = image.samples[0].sourceextent.clone();
            image.samples[0].crop = generated::VisualRegion {
                x: 0,
                y: 0,
                width: 200,
                height: 200,
            };
            assert!(valid_validation(&image));
            for invalid in 0..5 {
                let mut changed = image.clone();
                match invalid {
                    0 => changed.frame.sourceextent.width = 0,
                    1 => changed.samples[0].sourceextent.height += 1,
                    2 => changed.samples[0].content.x = u32::MAX,
                    3 => changed.samples[0].content.height += 1,
                    _ => changed.samples[1].content.width = 201,
                }
                assert!(!valid_validation(&changed));
            }
        }
    }

    #[test]
    fn validation_geometry_requires_the_selected_retained_sample() {
        let mut image = crate::view_model::test_support::validation_image_metadata();
        assert!(valid_validation(&image));
        image.detail = true;
        assert!(!valid_validation(&image));
        image.selected = Some(image.samples[0].identity.clone());
        image.frame.extent = image.samples[0].pixelextent.clone();
        image.frame.content = image.samples[0].content.clone();
        image.frame.sourceextent = image.samples[0].sourceextent.clone();
        image.samples[0].crop = image.frame.content.clone();
        image.samples[1].crop = generated::VisualRegion {
            x: 0,
            y: 0,
            width: 0,
            height: 0,
        };
        assert!(valid_validation(&image));
        image.samples[0].available = false;
        assert!(!valid_validation(&image));
        image.samples[0].available = true;
        image.samples[0].crop.width = image.frame.extent.width + 1;
        assert!(!valid_validation(&image));
    }

    #[test]
    fn validation_rejects_empty_and_outside_content_and_pairs_derived_identity() {
        let original = crate::view_model::test_support::validation_image_metadata();
        for content in [
            generated::VisualRegion {
                x: 0,
                y: 0,
                width: 0,
                height: 576,
            },
            generated::VisualRegion {
                x: 1,
                y: 0,
                width: 512,
                height: 576,
            },
            generated::VisualRegion {
                x: u32::MAX,
                y: 0,
                width: 2,
                height: 576,
            },
        ] {
            let mut invalid = original.clone();
            invalid.frame.content = content;
            assert!(!valid_validation(&invalid));
        }
        let mut source = original;
        source.detail = true;
        source.selected = Some(source.samples[0].identity.clone());
        source.frame.extent = source.samples[0].pixelextent.clone();
        source.frame.sourceextent = source.samples[0].sourceextent.clone();
        source.frame.resizemode = Some(generated::ImageResizeMode::Stretch);
        source.frame.content = generated::VisualRegion {
            x: 0,
            y: 0,
            width: 200,
            height: 200,
        };
        source.samples[0].crop = source.frame.content.clone();
        assert!(valid_validation(&source));
        let mut derived_frame = source.frame.clone();
        derived_frame.source.kind = generated::PresentationSourceKind::Upscale;
        derived_frame.resizemode = None;
        derived_frame.extent = generated::VisualExtent {
            width: 1600,
            height: 800,
        };
        derived_frame.content = generated::VisualRegion {
            x: 0,
            y: 0,
            width: 1600,
            height: 800,
        };
        let physical = crate::view_model::test_support::physical_frame(
            generated::presentation_source_session(generated::PresentationSourceKind::Upscale),
            derived_frame.revision,
            1,
            1600,
            800,
        );
        let derived = generated::UpscaleImageMetadata {
            frame: derived_frame.clone(),
            preparedextent: generated::VisualExtent { width: 400, height: 200 },
            preparedcontent: generated::VisualRegion { x: 0, y: 0, width: 400, height: 200 },
            input: source.frame.clone(),
            scene: crate::view_model::test_support::explore_snapshot().scene,
        };
        let bytes = encode(
            derived_frame.clone(),
            encode_product(generated::ApplicationSystem::Upscale, derived.clone()),
            Some(encode_product(
                generated::ApplicationSystem::Validation,
                source.clone(),
            )),
        );
        super::super::reset_test_releases();
        install(physical, 1600, 800, 1, &bytes).unwrap();
        retire(physical);
        for invalid in 0..5 {
            let mut changed = derived.clone();
            match invalid {
                0 => changed.frame.sourceextent.width = 0,
                1 => changed.frame.sourceextent.height += 1,
                2 => changed.frame.content.width -= 1,
                3 => changed.frame.content.x += 1,
                _ => changed.input.extent.width = u32::MAX,
            }
            let bytes = encode(
                changed.frame.clone(),
                encode_product(generated::ApplicationSystem::Upscale, changed),
                Some(encode_product(
                    generated::ApplicationSystem::Validation,
                    source.clone(),
                )),
            );
            assert!(install(physical, 1600, 800, 2, &bytes).is_err());
            assert!(pending(physical).is_none());
        }
        source.frame.revision += 1;
        let invalid = encode(
            derived_frame,
            encode_product(generated::ApplicationSystem::Upscale, derived),
            Some(encode_product(
                generated::ApplicationSystem::Validation,
                source,
            )),
        );
        assert!(install(physical, 1600, 800, 2, &invalid).is_err());
        super::super::reset_test_releases();
    }

    #[test]
    fn explore_preparation_is_shared_by_pending_and_completed_publications() {
        for referenced in [false, true] {
            super::super::reset_test_releases();
            let (model, frame) = crate::view_model::test_support::explore_presentation();
            let mut snapshot = model.explore.snapshot.as_ref().unwrap().clone();
            snapshot.scene.categories = (0..256)
                .map(|category| generated::ClassName {
                    value: format!("class {category}"),
                })
                .collect();
            snapshot.scene.objects = if referenced {
                vec![crate::view_model::test_support::annotation_object(255); 2]
            } else {
                Vec::new()
            };
            install_explore(frame, &snapshot);
            let mut candidate = pending(frame).unwrap();
            let detail = candidate.content.detail().unwrap();
            let cloned = candidate.content.clone().detail().unwrap();
            assert_eq!(detail.labels.len(), usize::from(referenced));
            assert!(Arc::ptr_eq(&detail.labels, &cloned.labels));
            assert!(Arc::ptr_eq(&detail.explore, &cloned.explore));
            assert!(super::super::accept_publication(frame));
            let read = super::super::SampleRead::acquire(frame).unwrap();
            candidate.read = Some(read.clone());
            candidate.complete = false;
            let surface = candidate.surface;
            let mut image = super::super::ImagePublication {
                surface,
                completed: None,
                retained_read: None,
                pending_sample: Some(candidate),
                content: Content::default(),
                placement: Placement::Contain,
            };
            super::super::authorize_draw(Some(frame));
            assert!(image.submitted_draw(surface).is_some());
            assert!(!image.promote(frame, &model));
            image.complete(frame);
            assert!(image.promote(frame, &model));
            let completed = image.content.detail().unwrap();
            assert!(Arc::ptr_eq(&detail.labels, &completed.labels));
            assert!(Arc::ptr_eq(&detail.explore, &completed.explore));
            assert_eq!(Arc::strong_count(&read), 2);

            // New physical storage with unchanged numeric content identities
            // still owns its independently prepared immutable metadata.
            let replacement = FrameReady {
                slot: 1,
                presentation_revision: frame.presentation_revision + 1,
                ..frame
            };
            snapshot.scene.categories[255].value = "replacement".into();
            install_explore(replacement, &snapshot);
            let mut next = pending(replacement).unwrap();
            let next_detail = next.content.detail().unwrap();
            assert_eq!(next_detail.labels.len(), usize::from(referenced));
            assert!(!Arc::ptr_eq(&detail.labels, &next_detail.labels));
            assert_eq!(
                next_detail.explore.scene.categories[255].value,
                "replacement"
            );
            assert_eq!(detail.explore.scene.categories[255].value, "class 255");
            assert!(super::super::accept_publication(replacement));
            let next_read = super::super::SampleRead::acquire(replacement).unwrap();
            next.read = Some(next_read.clone());
            next.complete = false;
            let next_surface = next.surface;
            image.pending_sample = Some(next);
            assert!(!image.promote(replacement, &model));
            assert_eq!(image.retained().unwrap().frame, Some(frame));
            assert!(Arc::ptr_eq(
                &image.content.detail().unwrap().labels,
                &detail.labels
            ));
            super::super::authorize_draw(Some(replacement));
            let encoded_draw = image.submitted_draw(next_surface).unwrap().clone();
            assert!(Arc::ptr_eq(
                &encoded_draw.content.detail().unwrap().labels,
                &next_detail.labels
            ));
            image.complete(replacement);
            assert!(image.promote(replacement, &model));
            assert!(Arc::ptr_eq(
                &image.content.detail().unwrap().labels,
                &next_detail.labels
            ));
            assert!(super::super::test_releases().is_empty());
            super::super::retire_publication(frame);
            super::super::retire_publication(replacement);
            retire(frame);
            retire(replacement);
            assert!(super::super::test_releases().is_empty());
            drop(read);
            assert_eq!(super::super::test_releases(), vec![frame]);
            drop(image);
            drop(next_read);
            assert_eq!(super::super::test_releases(), vec![frame]);
            drop(encoded_draw);
            assert_eq!(super::super::test_releases(), vec![frame, replacement]);
            // Caption handles survive both releases without holding pixels.
            assert_eq!(detail.labels.len(), usize::from(referenced));
            assert_eq!(next_detail.labels.len(), usize::from(referenced));
            super::super::reset_test_releases();
        }
    }

    #[test]
    fn accepted_first_validation_product_shares_metadata_and_cached_content() {
        super::super::reset_test_releases();
        let product = crate::view_model::test_support::validation_image_metadata();
        let frame = crate::view_model::test_support::physical_frame(
            generated::presentation_source_session(generated::PresentationSourceKind::Validation),
            product.frame.revision,
            1,
            product.frame.extent.width,
            product.frame.extent.height,
        );
        let bytes = encode(
            product.frame.clone(),
            encode_product(generated::ApplicationSystem::Validation, product),
            None,
        );
        install(frame, frame.content_width, frame.content_height, 1, &bytes).unwrap();
        let surface = surface(frame).unwrap();
        assert!(super::super::drawable_validation(surface).is_none());
        assert!(super::super::accept_publication(frame));
        let (_, first) =
            super::super::drawable_validation(surface).expect("initial shader admission");
        let (_, next) = super::super::drawable_validation(surface).unwrap();
        assert!(Arc::ptr_eq(&first, &next));
        let pending = pending(frame).unwrap();
        let WorkspaceImageProduct::Validation(native) = pending.content.product.as_ref().unwrap()
        else {
            panic!("wrong native product")
        };
        assert!(Arc::ptr_eq(native, &first.metadata));
        assert_eq!(first.metadata.samples[0].identity.generation, 7);
        assert!(super::super::drawable_prediction(surface).is_none());
        retire(frame);
        assert!(super::super::drawable_validation(surface).is_none());
        super::super::reset_test_releases();
    }
}
