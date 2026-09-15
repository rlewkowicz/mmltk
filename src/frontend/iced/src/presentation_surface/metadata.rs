use super::{AnnotationContent, DetailContent, FrameReady, PendingImage, Placement, Surface};
use crate::application_codec::FromApplicationValue;
use crate::generated::{self, WorkspaceImageProduct};
use std::{cell::RefCell, sync::Arc};

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
    let region = &frame.content;
    frame.source.instance != 0
        && frame.revision != 0
        && region.width != 0
        && region.height != 0
        && region
            .x
            .checked_add(region.width)
            .is_some_and(|end| end <= frame.extent.width)
        && region
            .y
            .checked_add(region.height)
            .is_some_and(|end| end <= frame.extent.height)
}

fn valid_detail(source: &generated::ExploreImageMetadata) -> bool {
    source.mode == generated::ExploreMode::Detail
        && source.dataset.identity != 0
        && source.selectedimage.is_some()
        && valid_content(&source.frame)
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
    let mut gallery = None;
    let mut detail = None;
    let mut annotation = None;
    let mut prediction = None;
    let mut validation = None;
    match product {
        WorkspaceImageProduct::Explore(snapshot) if snapshot.frame == metadata.frame => {
            let snapshot = Arc::new(snapshot);
            if snapshot.mode == generated::ExploreMode::Gallery {
                if !super::gallery::valid_layout(&snapshot) {
                    return Err("invalid graphics atlas metadata".into());
                }
                gallery = Some(snapshot);
            } else {
                if !valid_detail(&snapshot) {
                    return Err("invalid graphics detail metadata".into());
                }
                detail = Some(DetailContent {
                    explore: snapshot,
                    upscale: None,
                });
            }
        }
        WorkspaceImageProduct::Annotation(snapshot) if snapshot.frame == metadata.frame => {
            annotation = Some(AnnotationContent {
                diagnostics: snapshot.diagnostics.map(Arc::new),
            });
        }
        WorkspaceImageProduct::Upscale(snapshot) if snapshot.frame == metadata.frame => {
            let Some(WorkspaceImageProduct::Explore(source)) = source else {
                return Err("missing graphics detail source metadata".into());
            };
            if source.frame != snapshot.input
                || !valid_detail(&source)
                || !valid_content(&snapshot.frame)
            {
                return Err("graphics detail source identity mismatch".into());
            }
            detail = Some(DetailContent {
                explore: Arc::new(source),
                upscale: Some(Arc::new(snapshot)),
            });
        }
        WorkspaceImageProduct::Predict(snapshot) if snapshot.frame == metadata.frame && snapshot.contentidentity != 0 => {
            prediction = Some(Arc::new(super::labels::PredictionContent::new(snapshot)));
        }
        WorkspaceImageProduct::Validation(snapshot) if snapshot.frame == metadata.frame && snapshot.contentidentity != 0 => {
            if snapshot.samples.iter().any(|sample| sample.available &&
                (sample.identity.generation == 0 || sample.originalextent.width == 0 || sample.originalextent.height == 0 ||
                 sample.crop.width == 0 || sample.crop.height == 0 ||
                 sample.crop.x.checked_add(sample.crop.width).is_none_or(|end| end > snapshot.frame.extent.width) ||
                 sample.crop.y.checked_add(sample.crop.height).is_none_or(|end| end > snapshot.frame.extent.height))) {
                return Err("invalid validation sample image geometry".into());
            }
            validation = Some(Arc::new(super::labels::ValidationContent::new(snapshot)));
        }
        WorkspaceImageProduct::Live(snapshot) if snapshot.frame == metadata.frame => {}
        _ => return Err("graphics metadata does not describe this visual product".into()),
    }
    let surface = Surface {
        high: frame.high,
        low: frame.low,
        width,
        height,
        frame: Some(frame),
        crop: None,
        viewer_identity: prediction.as_ref().map(|content: &Arc<super::labels::PredictionContent>|
            (frame.content_session, content.metadata.contentidentity)).or_else(|| validation.as_ref().map(|content: &Arc<super::labels::ValidationContent>|
            (frame.content_session, content.metadata.contentidentity))),
        fit_revision: 0,
    };
    let placement = gallery.as_ref().map_or(Placement::Contain, |snapshot| {
        super::gallery::placement(snapshot)
    });
    let image = Image {
        frame,
        product: metadata.frame,
        pending: PendingImage {
            read: None,
            surface,
            gallery,
            detail,
            annotation,
            prediction,
            validation,
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
        if let Some(gallery) = image.pending.gallery.as_deref() {
            super::trace_gallery_source(gallery);
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
