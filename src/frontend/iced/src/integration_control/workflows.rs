//! Real model workflows driven through the packaged Iced interface.
use super::annotation_checks::annotation_layout_scale;
use crate::generated::FeatureId;
use crate::generated::{ComputeOperationOutcome, SourceKind};
use crate::integration_control::pixel_checks::ProbeOutcome;
#[cfg(target_arch = "wasm32")]
use crate::integration_control::pixel_checks::pixel_result_callback;
use crate::integration_control::widget_ops::click;
use crate::integration_control::widget_ops::{AnnotationReveal, locate, reveal_control};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::workflow_pixels_js;
use crate::integration_control::{Driver, Phase, reporting, widget_ops};
#[cfg(target_arch = "wasm32")]
use crate::integration_control::{Message, probe::scenario_output};
use crate::message::Message as RootMessage;
use crate::view::{settings, workflow};
use crate::view_model::ApplicationModel;
use iced::{Rectangle, Task};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Picture {
    Progress,
    Train,
    Validation,
    Confidence,
    Gallery,
    Detail,
    Compiled,
    Image,
    Video,
    Stop,
    Theme,
    Narrow,
}
impl Picture {
    fn name(self) -> &'static str {
        match self {
            Self::Progress => "progress",
            Self::Train => "train",
            Self::Validation => "validation",
            Self::Confidence => "confidence",
            Self::Gallery => "validate-to-explore",
            Self::Detail => "detail",
            Self::Compiled => "compiled",
            Self::Image => "image",
            Self::Video => "video",
            Self::Stop => "stop",
            Self::Theme => "theme",
            Self::Narrow => "narrow",
        }
    }
    fn chart(self) -> bool {
        matches!(self, Self::Train | Self::Theme | Self::Narrow)
    }
    fn control(self, _index: u8) -> String {
        match self {
            Self::Progress => "train.progress.bar".into(),
            Self::Train | Self::Theme | Self::Narrow => "train.metrics.plot".into(),
            Self::Validation | Self::Confidence => crate::view::validate::samples::ATLAS_ID.into(),
            Self::Detail => "validate.detail.image".into(),
            Self::Gallery => crate::view::explore::GALLERY_WORKSPACE_ID.into(),
            _ => "workflow.visual.workspace".into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Retention {
    Expanded,
    Back,
    Hidden,
    Revealed,
    Navigation,
}
impl Retention {
    fn name(self) -> &'static str {
        match self {
            Self::Expanded => "expanded",
            Self::Back => "back",
            Self::Hidden => "hidden",
            Self::Revealed => "revealed",
            Self::Navigation => "navigation",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Step {
    Train,
    StartTrain,
    Training,
    LeaveTrain,
    HiddenTrain,
    ReturnTrain,
    Trained,
    NoImageWorkspace,
    TrainAspect,
    ChartBounds,
    ChartTile(u8),
    ChartLegend,
    ChartLegendPending,
    ChartLegendChanged,
    ChartPan,
    ChartPanPending,
    ChartPanned,
    ChartSettle(Retention),
    ChartSettlePending(Retention),
    ChartRetained(Retention),
    ChartSelector,
    ChartHide,
    ChartAbsent,
    ChartShow,
    ChartCloseSelector,
    ChartLeave,
    ChartAway,
    ChartReturn,
    ExpandChart,
    ExpandedBounds,
    ExpandedChart,
    ChartWheel(u8, bool),
    ChartWheelPending(u8, bool),
    ChartWheelSettle(u8, bool),
    ChartWheelSettling(u8, bool),
    ChartScrolled(u8, bool),
    BackToCharts,
    ChartAspect(u8),
    ChartAspectReady(u8),
    PrepareExplore,
    OpenGallery,
    GalleryReady,
    Validate,
    StartValidate,
    Validating,
    NoValidationAspect,
    ConfidenceEdit(u8),
    ConfidenceReady(u8),
    ConfidenceLayer(bool),
    ConfidenceLayerReady(bool),
    ConfidenceLayoutResize,
    ConfidenceLayoutReady,
    ConfidenceLayoutRestore,
    OpenSample,
    Sample,
    ValidationOriginal,
    ValidationOriginalReady,
    HideBoxes,
    HiddenBoxes,
    ValidationLayer(bool, u8),
    ValidationLayerReady(bool, u8),
    CloseSample,
    ClosedSample,
    Explore,
    Gallery,
    Predict,
    Source(u8),
    SourceReady(u8),
    StartPredict(u8),
    Predicting(u8),
    Pause,
    Paused,
    Resume,
    VideoEnd,
    Restart,
    Restarted,
    Stop,
    Stopped,
    Export,
    PrepareExport,
    StartExport,
    Exporting,
    StopExport,
    ExportStopped,
    ExportReturn,
    Theme,
    Dark,
    DarkReady,
    Narrow,
    NarrowReady,
    Pixels(Picture, u8),
    AwaitPixels(Picture, u8),
}

#[derive(Default)]
pub(super) struct State {
    chart_bounds: Rectangle,
    chart_view: Option<crate::view::metrics::ChartView>,
    chart_sequence: Option<u64>,
    wheel_y: f32,
    hidden_sequence: u64,
    generation: u64,
    video_index: u64,
    narrow_scale: f32,
    pixel_source: u64,
    pixel_presentation: u64,
    pixel_attempts: u8,
    progress_epoch: Option<u64>,
    validation_layer: u8,
    confidence_delivered: bool,
    confidence_facts: Vec<f64>,
    confidence_metrics: Option<crate::generated::EvalSummary>,
    confidence_generation: u64,
    caption_patches: Vec<f64>,
    caption_receipt: Option<super::probe::ProbeReceipt>,
    gallery_tile: Vec<f64>,
    validation_original: u8,
    validation_frame: Option<crate::presentation_surface::Surface>,
    export_pixels: bool,
    primary_pixels: [bool; 4],
    primary_reveal: [Option<(u64, u64)>; 4],
    work_progress: Option<(FeatureId, u64, u64)>,
    export_narrow: bool,
}

fn ready_gallery_tile(model: &ApplicationModel) -> Option<(super::probe::ProbeReceipt, [f64; 5])> {
    let (surface, content) = crate::presentation_surface::gallery::displayed()?;
    let current = model.explore.snapshot.as_ref()?;
    if model.foreground_visual() != Some(crate::generated::PresentationSourceKind::Explore)
        || current.mode != crate::generated::ExploreMode::Gallery
        || content.metadata.frame != current.frame
        || content.metadata.dataset.identity != current.dataset.identity
        || content.metadata.order.matchingcount == 0
    {
        return None;
    }
    let receipt = super::probe::current_receipt(crate::view::explore::GALLERY_WORKSPACE_ID)?;
    if receipt.surface.frame != surface.frame {
        return None;
    }
    let draw = super::pixel_checks::AtlasDraw {
        surface,
        snapshot: content.metadata.clone(),
        bounds: receipt.bounds,
        image: receipt.image,
        clip: receipt.clip,
    };
    let (compiled, tile) = draw
        .ready_tiles()
        .find(|(_, tile)| tile.width >= 8.0 && tile.height >= 8.0)?;
    Some((
        receipt,
        [
            f64::from(compiled),
            f64::from(tile.x),
            f64::from(tile.y),
            f64::from(tile.width),
            f64::from(tile.height),
        ],
    ))
}

fn atlas_cell(bounds: Rectangle, index: u8) -> Rectangle {
    Rectangle {
        x: bounds.x + f32::from(index % 2) * bounds.width / 2.0,
        y: bounds.y + f32::from(index / 2) * bounds.height / 3.0,
        width: bounds.width / 2.0,
        height: bounds.height / 3.0,
    }
}
fn layer_selection(index: u8) -> (bool, bool) {
    match index % 4 {
        1 => (false, true),
        2 => (false, false),
        3 => (true, false),
        _ => (true, true),
    }
}

// Independent native-metadata oracle: retain at most eight small intersections
// of GT and Det caption interiors. Last-painted captions exclude occluders.
// The browser compares actual Det-only pixels with the later combined layer.
fn validation_caption_patches(
    content: &crate::presentation_surface::labels::ValidationContent,
    receipt: &super::probe::ProbeReceipt,
    sample_index: u8,
    patches: &mut Vec<f64>,
) {
    patches.clear();
    let scale = receipt.scale;
    if !scale.is_finite() || scale <= 0.0 {
        return;
    }
    let metadata = &content.metadata;
    let sample = if metadata.detail {
        metadata
            .samples
            .iter()
            .find(|sample| metadata.selected.as_ref() == Some(&sample.identity))
    } else {
        metadata.samples.get(usize::from(sample_index))
    };
    let Some(sample) = sample.filter(|sample| sample.available) else {
        return;
    };
    let [crop_x, crop_y, crop_width, crop_height] = receipt.surface.content_region();
    if crop_width == 0 || crop_height == 0 {
        return;
    }
    let frame = content.frame();
    let sx = frame.extent.width as f32 / metadata.frame.extent.width as f32;
    let sy = frame.extent.height as f32 / metadata.frame.extent.height as f32;
    let cell = if metadata.detail {
        receipt.image
    } else {
        let width = metadata.frame.extent.width / 2;
        let height = metadata.frame.extent.height / 3;
        Rectangle {
            x: receipt.image.x
                + (f32::from(sample_index % 2) * width as f32 * sx - crop_x as f32)
                    * receipt.image.width
                    / crop_width as f32,
            y: receipt.image.y
                + (f32::from(sample_index / 2) * height as f32 * sy - crop_y as f32)
                    * receipt.image.height
                    / crop_height as f32,
            width: width as f32 * sx * receipt.image.width / crop_width as f32,
            height: height as f32 * sy * receipt.image.height / crop_height as f32,
        }
    };
    let Some(clip) = cell.intersection(&receipt.clip) else {
        return;
    };
    let layers: [Vec<_>; 2] = std::array::from_fn(|layer| {
        sample
            .labels
            .iter()
            .rev()
            .filter(|label| label.groundtruth == (layer == 0))
            .filter_map(|label| {
                let x = (sample.crop.x as f32
                    + label.box_.first.x * sample.crop.width as f32
                        / sample.pixelextent.width as f32)
                    * sx;
                let y = (sample.crop.y as f32
                    + label.box_.first.y * sample.crop.height as f32
                        / sample.pixelextent.height as f32)
                    * sy;
                let bounds = Rectangle {
                    x: receipt.image.x
                        + (x - crop_x as f32) * receipt.image.width / crop_width as f32,
                    y: receipt.image.y
                        + (y - crop_y as f32) * receipt.image.height / crop_height as f32,
                    width: (label.name.chars().count() as f32 * 7.5 + 8.0).max(20.0) * scale,
                    height: 19.0 * scale,
                };
                bounds
                    .intersection(&clip)
                    .map(|_| (bounds, label.rgb.0, !label.name.trim().is_empty()))
            })
            .take(16)
            .collect()
    });
    for (det_index, (det, det_rgb, _)) in layers[1].iter().enumerate() {
        for (gt_index, (gt, gt_rgb, has_text)) in layers[0].iter().enumerate() {
            let Some(mut interior) = det
                .intersection(gt)
                .and_then(|value| value.intersection(&clip))
            else {
                continue;
            };
            interior.x += scale;
            interior.y += scale;
            interior.width -= 2.0 * scale;
            interior.height -= 2.0 * scale;
            if !*has_text || interior.width < 4.0 || interior.height < 4.0 {
                continue;
            }
            // Public label layout: a left-aligned paragraph starts three logical
            // pixels inside the quad, has width (quad - 6), and is centered at
            // y + 9.5. Keep the bounded read on that text region on both axes;
            // the independent browser oracle still requires actual glyph pixels.
            let text_left = gt.x + 3.0 * scale;
            let text_width = gt.width - 6.0 * scale;
            let target = iced::Point::new(text_left + text_width / 2.0, gt.y + 9.5 * scale);
            if !interior.contains(target) {
                continue;
            }
            let width = interior.width.min(128.0);
            let height = interior.height.min(15.0);
            let patch = Rectangle {
                x: (target.x - width / 2.0).clamp(interior.x, interior.x + interior.width - width),
                y: (target.y - height / 2.0)
                    .clamp(interior.y, interior.y + interior.height - height),
                width,
                height,
            };
            // Browser rounding must retain the target too, not merely a legal
            // sliver of background beside it. Occlusion uses the final patch.
            if target.x < patch.x.ceil()
                || target.x >= (patch.x + width).floor()
                || target.y < patch.y.ceil()
                || target.y >= (patch.y + height).floor()
                || layers[0][..gt_index]
                    .iter()
                    .chain(&layers[1][..det_index])
                    .any(|(later, _, _)| later.intersection(&patch).is_some())
            {
                continue;
            }
            patches.extend([patch.x, patch.y, patch.width, patch.height].map(f64::from));
            patches.extend(gt_rgb.map(f64::from));
            patches.extend(det_rgb.map(f64::from));
            if patches.len() == 80 {
                return;
            }
        }
    }
    if patches.is_empty() {
        reporting::emit(|sink| {
            sink.record(
                "integration.workflow.caption_geometry",
                receipt.control,
                "clip",
                [clip.x, clip.y, clip.width, clip.height].map(f64::from),
            );
            for (layer, labels) in layers.iter().enumerate() {
                for (bounds, _, has_text) in labels {
                    sink.record(
                        "integration.workflow.caption_geometry",
                        receipt.control,
                        &format!("layer={layer} text={has_text} scale={scale}"),
                        [bounds.x, bounds.y, bounds.width, bounds.height].map(f64::from),
                    );
                }
            }
        });
    }
}

fn source(index: u8) -> SourceKind {
    match index {
        0 => SourceKind::CompiledDataset,
        1 => SourceKind::SingleImage,
        _ => SourceKind::VideoFile,
    }
}
fn primary(feature: FeatureId) -> &'static str {
    workflow::Composition::new(feature, 0.0).stable_id(workflow::Region::PrimaryAction)
}
fn completed(stage: &str, facts: [f64; 4]) {
    reporting::emit(|sink| sink.record("integration.workflow.completed", "", stage, facts));
}

#[cfg(test)]
mod tests {
    use super::{Picture, Step};
    use crate::generated::FeatureId;
    use crate::integration_control::pixel_checks::ProbeOutcome;
    use crate::integration_control::{Controller, Message, Phase};

    fn advance_workflow(
        controller: &mut Controller,
        model: &crate::view_model::ApplicationModel,
        step: Step,
        feature: FeatureId,
    ) -> usize {
        controller
            .workflows
            .advance_workflows(
                &mut controller.widgets,
                &mut controller.driver,
                step,
                model,
                &crate::view::settings::SettingsModel::default(),
                feature,
                None,
                &crate::view::router::Router::default(),
            )
            .units()
    }

    #[test]
    fn caption_patch_oracle_uses_paired_native_bounds_and_explicit_rgb() {
        let mut metadata = crate::view_model::test_support::validation_image_metadata();
        let mut gt = metadata.samples[0].labels[0].clone();
        gt.name = "人é🙂".into();
        gt.rgb.0 = [0, 255, 255];
        let mut det = gt.clone();
        det.groundtruth = false;
        det.rgb.0 = [255, 0, 0];
        metadata.samples[0].labels = vec![det, gt];
        let bounds = iced::Rectangle::with_size(iced::Size::new(512.0, 576.0));
        let receipt = super::super::probe::ProbeReceipt {
            generation: 1,
            control: crate::view::validate::samples::ATLAS_ID,
            surface: crate::presentation_surface::Surface {
                width: 512,
                height: 576,
                ..crate::presentation_surface::Surface::empty()
            },
            bounds,
            image: bounds,
            clip: bounds,
            scale: 1.0,
        };
        let mut patches = Vec::new();
        for stage in 0..=8 {
            (
                metadata.overlays.groundtruthlayer,
                metadata.overlays.predictionlayer,
            ) = super::layer_selection(stage);
            let content =
                crate::presentation_surface::labels::ValidationContent::new(metadata.clone());
            super::validation_caption_patches(&content, &receipt, 0, &mut patches);
            assert_eq!(patches.len(), 10);
            assert!((patches[0] - 52.2).abs() < 0.001);
            assert!((patches[1] - 40.4).abs() < 0.001);
            assert!((patches[2] - 28.5).abs() < 0.001);
            assert_eq!(patches[3], 15.0);
            assert_eq!(&patches[4..], &[0.0, 255.0, 255.0, 255.0, 0.0, 0.0]);
        }
        for detail in [false, true] {
            metadata.detail = detail;
            metadata.selected = detail.then(|| metadata.samples[0].identity.clone());
            let content =
                crate::presentation_surface::labels::ValidationContent::new(metadata.clone());
            for (scale, expected_origin) in [
                (1.0, [61.0, 41.0]),
                (1.5, [91.5, 64.35]),
                (2.0, [122.0, 88.3]),
                (4.25, [259.25, 196.075]),
                (5.25, [320.25, 243.975]),
                (12.5, [766.625, 591.25]),
                (40.0, [2594.0, 1908.5]),
            ] {
                let scaled = super::super::probe::ProbeReceipt {
                    bounds: crate::presentation_surface::physical_bounds(bounds, scale),
                    image: crate::presentation_surface::physical_bounds(bounds, scale),
                    clip: crate::presentation_surface::physical_bounds(
                        iced::Rectangle {
                            x: 60.0,
                            y: 40.0,
                            width: 100.0,
                            height: 100.0,
                        },
                        scale,
                    ),
                    scale,
                    ..receipt.clone()
                };
                super::validation_caption_patches(&content, &scaled, 0, &mut patches);
                assert_eq!(patches.len(), 10);
                let factor = f64::from(scale);
                let expected = [
                    expected_origin[0],
                    expected_origin[1],
                    (19.7 * factor).min(128.0),
                    15.0,
                ];
                for (actual, expected) in patches[..4].iter().zip(expected) {
                    assert!((*actual - expected).abs() < 0.001);
                    assert!(actual.is_finite());
                }
                assert!(patches[2] <= 128.0 && patches[3] <= 15.0);
                assert!((patches[0] + patches[2]).floor() - patches[0].ceil() >= 3.0);
                assert!((patches[1] + patches[3]).floor() - patches[1].ceil() >= 3.0);
                // Independently known Unicode paragraph region: x=54.2..78.7,
                // vertical center y=47.9, before viewport scaling. Its center
                // must survive both physical capping and browser rounding.
                let target = [66.45 * factor, 47.9 * factor];
                for axis in 0..2 {
                    assert!(patches[axis].ceil() <= target[axis]);
                    assert!(target[axis] < (patches[axis] + patches[axis + 2]).floor());
                }
                assert!(patches[0] >= 61.0 * factor - 0.001);
                assert!(patches[1] >= 41.0 * factor - 0.001);
                assert!(patches[0] + patches[2] <= 80.7 * factor + 0.001);
                assert!(patches[1] + patches[3] <= 56.4 * factor + 0.001);
                assert_eq!(&patches[4..], &[0.0, 255.0, 255.0, 255.0, 0.0, 0.0]);
                for clip in [
                    iced::Rectangle {
                        x: 52.0,
                        y: 38.0,
                        width: 8.0,
                        height: 30.0,
                    },
                    iced::Rectangle {
                        x: 50.0,
                        y: 39.0,
                        width: 40.0,
                        height: 6.0,
                    },
                ] {
                    let excluded = super::super::probe::ProbeReceipt {
                        clip: crate::presentation_surface::physical_bounds(clip, scale),
                        ..scaled.clone()
                    };
                    super::validation_caption_patches(&content, &excluded, 0, &mut patches);
                    assert!(
                        patches.is_empty(),
                        "clip excludes the horizontal or vertical glyph target"
                    );
                }
            }
            for scale in [0.0, -1.0, f32::NAN, f32::INFINITY] {
                let invalid = super::super::probe::ProbeReceipt {
                    scale,
                    ..receipt.clone()
                };
                super::validation_caption_patches(&content, &invalid, 0, &mut patches);
                assert!(patches.is_empty());
            }
        }
        metadata.detail = false;
        for name in ["", "   "] {
            let mut blank = metadata.clone();
            blank.samples[0].labels[1].name = name.into();
            let content = crate::presentation_surface::labels::ValidationContent::new(blank);
            super::validation_caption_patches(&content, &receipt, 0, &mut patches);
            assert!(
                patches.is_empty(),
                "a blank GT paragraph has no glyph target"
            );
        }
        let content = crate::presentation_surface::labels::ValidationContent::new(metadata);
        super::validation_caption_patches(&content, &receipt, 5, &mut patches);
        assert!(patches.is_empty());
        super::validation_caption_patches(&content, &receipt, u8::MAX, &mut patches);
        assert!(patches.is_empty());
    }

    #[test]
    fn validation_controls_wait_for_the_sample_selection_reply() {
        for step in [
            Step::OpenSample,
            Step::HideBoxes,
            Step::ValidationLayer(true, 1),
            Step::CloseSample,
        ] {
            let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
            let controller = &mut fixture.controller;
            controller.driver.phase = Phase::Workflows(step);
            let mut model = crate::view_model::test_support::bootstrapped();
            model.workflow.validation.as_mut().unwrap().detail = true;
            let pending = model
                .begin_intent(crate::generated::ApplicationIntentEndpoint::ValidationSelectSample)
                .unwrap();
            assert_eq!(
                advance_workflow(controller, &model, step, FeatureId::Validate),
                0
            );
            assert!(!controller.widgets.location_pending());
            model.abandon_intent(pending);
            assert!(advance_workflow(controller, &model, step, FeatureId::Validate) > 0);
        }
    }

    #[test]
    fn active_primary_reveal_yields_to_rendering_until_native_progress_changes() {
        for (feature, step) in [
            (FeatureId::Train, Step::Training),
            (FeatureId::Export, Step::Exporting),
        ] {
            let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
            let controller = &mut fixture.controller;
            controller.driver.phase = Phase::Workflows(step);
            let mut model = crate::view_model::test_support::bootstrapped();
            let operation = match feature {
                FeatureId::Train => &mut model.workflow.training.as_mut().unwrap().local,
                _ => model.workflow.export.as_mut().unwrap(),
            };
            operation.active = true;
            assert!(advance_workflow(controller, &model, step, feature) > 0);
            controller.widgets.location_completed();
            assert_eq!(advance_workflow(controller, &model, step, feature), 0);
            assert_eq!(advance_workflow(controller, &model, step, feature), 0);
            let operation = match feature {
                FeatureId::Train => &mut model.workflow.training.as_mut().unwrap().local,
                _ => model.workflow.export.as_mut().unwrap(),
            };
            operation.progress.sequence += 1;
            assert!(advance_workflow(controller, &model, step, feature) > 0);
        }
    }

    #[test]
    fn page_navigation_waits_for_verified_idle_primary_pixels() {
        let mut fixture = crate::integration_control::ProbeFixture::new("");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::PagePrimary(FeatureId::Validate);
        let model = crate::view_model::test_support::bootstrapped();
        let settings = crate::view::settings::SettingsModel::default();
        let advance = |controller: &mut Controller| {
            controller
                .lifecycle
                .advance_lifecycle(
                    &mut controller.driver,
                    &mut controller.widgets,
                    &model,
                    &settings,
                    1.0,
                    FeatureId::Validate,
                )
                .units()
        };
        assert!(advance(controller) > 0);
        let _ = controller.update_location(
            "validate.primary".into(),
            iced::Rectangle {
                x: 10.0,
                y: 10.0,
                width: 200.0,
                height: 48.0,
            },
        );
        assert_eq!(
            controller.driver.phase,
            Phase::AwaitPagePrimary(FeatureId::Validate)
        );
        assert_eq!(advance(controller), 0);
        let completion = |control: &str, active| Message::Scoped {
            generation: controller.driver.generation,
            receipt: None,
            message: Box::new(Message::PrimaryActionPixels {
                control: control.into(),
                active,
                token: 1,
            }),
        };
        let active = completion("validate.primary", true);
        let other = completion("train.primary", false);
        let idle = completion("validate.primary", false);
        let _ = controller.update(active);
        let _ = controller.update(other);
        assert_eq!(advance(controller), 0);
        let _ = controller.update(idle);
        let _ = advance(controller);
        assert_eq!(
            controller.driver.phase,
            Phase::PageNavigation(FeatureId::Predict)
        );
    }

    #[test]
    fn export_stop_requires_current_active_primary_canvas_completion() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::Exporting);
        let generation = controller.driver.generation;
        let completion = |generation, active| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::PrimaryActionPixels {
                control: "export.primary".into(),
                active,
                token: 1,
            }),
        };
        let _ = controller.update(completion(generation.wrapping_sub(1), true));
        assert!(!controller.workflows.export_pixels);
        let _ = controller.update(completion(generation, false));
        assert!(!controller.workflows.export_pixels);
        let _ = controller.update(completion(generation, true));
        assert!(controller.workflows.export_pixels);
    }

    #[test]
    fn primary_measurement_tasks_belong_to_the_current_enabled_scenario() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::Exporting);
        let generation = controller.driver.generation;
        let request = |generation| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::PrimaryActionMeasure {
                control: "export.primary".into(),
                token: 1,
            }),
        };
        let _ = controller.update(request(generation.wrapping_sub(1)));
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_none()
        );
        let _ = controller.update(request(generation));
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_some()
        );
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_none()
        );
        crate::integration_control::initialize_reporting(false, false);
        let _ = controller.update(request(generation));
        assert!(
            controller
                .driver
                .reporting
                .primary_measurements(generation)
                .is_none()
        );
        assert!(
            !crate::integration_control::reporting::primary_action_current("export.primary", 1)
        );
    }

    #[test]
    fn chart_input_completion_requires_current_scenario_and_successful_settlement() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::ChartPanPending);
        let completion = |generation, delivered| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::ChartInputDelivered(delivered)),
        };
        let generation = controller.driver.generation;
        let _ = controller.update(completion(generation.wrapping_sub(1), true));
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::ChartPanPending)
        );
        let _ = controller.update(completion(generation, true));
        assert_eq!(controller.driver.phase, Phase::Workflows(Step::ChartPanned));
        controller.driver.phase = Phase::Workflows(Step::ChartWheelSettling(1, true));
        let _ = controller.update(completion(generation, true));
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::ChartScrolled(1, true))
        );
        controller.driver.phase = Phase::Workflows(Step::ChartLegendPending);
        let _ = controller.update(completion(generation, false));
        assert_eq!(controller.driver.phase, Phase::Failed);
    }

    #[test]
    fn confidence_delivery_retains_scenario_and_stage_ownership() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::ConfidenceReady(0));
        let generation = controller.driver.generation;
        let delivered = |generation, stage, success| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::ConfidenceInputDelivered(stage, success)),
        };
        let _ = controller.update(delivered(generation.wrapping_sub(1), 0, true));
        let _ = controller.update(delivered(generation, 1, true));
        assert!(!controller.workflows.confidence_delivered);
        let _ = controller.update(delivered(generation, 0, true));
        assert!(controller.workflows.confidence_delivered);
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::ConfidenceReady(0))
        );
        let _ = controller.update(delivered(generation, 0, false));
        assert_eq!(controller.driver.phase, Phase::Failed);
    }

    #[test]
    fn workflow_completion_requires_current_nonempty_canvas_evidence() {
        let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
        let controller = &mut fixture.controller;
        controller.driver.phase = Phase::Workflows(Step::AwaitPixels(Picture::Narrow, 0));
        let capture = |generation, outcome| Message::Scoped {
            generation,
            receipt: None,
            message: Box::new(Message::WorkflowPixels {
                picture: Picture::Narrow,
                index: 0,
                outcome,
            }),
        };
        let _ = controller.update(capture(
            controller.driver.generation.wrapping_sub(1),
            ProbeOutcome::Observed(256, 256),
        ));
        assert_eq!(
            controller.driver.phase,
            Phase::Workflows(Step::AwaitPixels(Picture::Narrow, 0))
        );
        let _ = controller.update(capture(
            controller.driver.generation,
            ProbeOutcome::Observed(256, 0),
        ));
        assert_eq!(controller.driver.phase, Phase::Failed);
    }

    #[test]
    fn progress_capture_rejects_a_training_phase_change_without_accepting_blank_pixels() {
        for changed in [false, true] {
            let mut fixture = crate::integration_control::ProbeFixture::new("workflows");
            let controller = &mut fixture.controller;
            controller.driver.phase = Phase::Workflows(Step::AwaitPixels(Picture::Progress, 0));
            let mut model = crate::view_model::test_support::bootstrapped();
            let train = model.workflow.training.as_mut().unwrap();
            train.local.active = true;
            train.metrics = Some(crate::view::metrics::tests::record());
            let progress = &mut train.metrics.as_mut().unwrap().progress;
            controller.workflows.progress_epoch = Some(progress.epoch as u64);
            if changed {
                progress.phase = crate::generated::TrainingPhase::Validate;
            }
            let _ = controller.workflows.advance_workflows(
                &mut controller.widgets,
                &mut controller.driver,
                Step::AwaitPixels(Picture::Progress, 0),
                &model,
                &crate::view::settings::SettingsModel::default(),
                FeatureId::Train,
                None,
                &crate::view::router::Router::default(),
            );
            controller.workflows.workflow_pixels(
                &mut controller.driver,
                Picture::Progress,
                0,
                ProbeOutcome::Observed(256, 0),
            );
            assert_eq!(
                controller.driver.phase,
                if changed {
                    Phase::Workflows(Step::Training)
                } else {
                    Phase::Failed
                }
            );
        }
    }
}

impl State {
    #[cfg(any(target_arch = "wasm32", test))]
    pub(super) fn confidence_input_delivered(
        &mut self,
        driver: &mut Driver,
        stage: u8,
        delivered: bool,
    ) {
        if driver.phase != Phase::Workflows(Step::ConfidenceReady(stage)) {
            return;
        }
        if delivered {
            self.confidence_delivered = true;
        } else {
            driver.fail("Validation confidence input was not delivered");
        }
    }

    pub(super) fn wheel_delivered(&mut self, driver: &mut Driver) {
        if let Phase::Workflows(Step::ChartWheelPending(index, expanded)) = driver.phase {
            driver.phase = Phase::Workflows(Step::ChartWheelSettle(index, expanded));
        }
    }

    pub(super) fn chart_input_delivered(&mut self, driver: &mut Driver) {
        let Phase::Workflows(step) = driver.phase else {
            return;
        };
        let next = match step {
            Step::ChartLegendPending => Step::ChartLegendChanged,
            Step::ChartPanPending => Step::ChartPanned,
            Step::ChartSettlePending(retention) => Step::ChartRetained(retention),
            Step::ChartWheelSettling(index, expanded) => Step::ChartScrolled(index, expanded),
            _ => return,
        };
        driver.phase = Phase::Workflows(next);
    }

    fn retained(
        &self,
        driver: &mut Driver,
        view: &crate::view::metrics::ChartView,
        stage: &str,
    ) -> bool {
        let Some(expected) = &self.chart_view else {
            driver.fail("Missing chart view baseline");
            return false;
        };
        if view.ranges != expected.ranges || view.legend_collapsed != expected.legend_collapsed {
            driver.fail(&format!("Chart camera or legend changed during {stage}"));
            return false;
        }
        reporting::chart_view(stage, view);
        true
    }

    pub(super) fn primary_action_pixels(&mut self, driver: &Driver, control: &str, active: bool) {
        if !active {
            return;
        }
        for (index, feature) in [
            FeatureId::Train,
            FeatureId::Validate,
            FeatureId::Predict,
            FeatureId::Export,
        ]
        .into_iter()
        .enumerate()
        {
            if control == primary(feature) {
                self.primary_pixels[index] = true;
            }
        }
        if driver.phase == Phase::Workflows(Step::Exporting)
            && control == primary(FeatureId::Export)
        {
            self.export_pixels = true;
        }
    }

    pub(super) fn workflow_step(&mut self, driver: &mut Driver, step: Step) -> Task<RootMessage> {
        driver.advance_to(Phase::Workflows(step))
    }
    pub(super) fn workflow_control(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        control: impl Into<String>,
    ) -> Task<RootMessage> {
        if !widgets.begin_location() {
            return Task::none();
        }
        let control = control.into();
        if matches!(
            driver.phase,
            Phase::Workflows(
                Step::Train
                    | Step::LeaveTrain
                    | Step::ReturnTrain
                    | Step::ChartLeave
                    | Step::ChartReturn
                    | Step::Validate
                    | Step::PrepareExplore
                    | Step::OpenGallery
                    | Step::Explore
                    | Step::Pixels(Picture::Gallery, _)
                    | Step::Predict
                    | Step::Export
                    | Step::ExportReturn
                    | Step::Theme
                    | Step::NoImageWorkspace
                    | Step::TrainAspect
                    | Step::NoValidationAspect
            )
        ) {
            // Navigation and Explore use their own layout/scroll owners;
            // absent-control checks also need the unmodified tree result.
            locate(control, driver.generation)
        } else {
            reveal_control(control, driver.generation, AnnotationReveal::Control)
        }
    }
    pub(super) fn workflow_pixels(
        &mut self,
        driver: &mut Driver,
        picture: Picture,
        index: u8,
        outcome: ProbeOutcome,
    ) {
        if driver.phase != Phase::Workflows(Step::AwaitPixels(picture, index)) {
            return;
        }
        if picture == Picture::Progress && self.progress_epoch.is_none() {
            self.retry_progress_capture(driver);
            return;
        }
        match outcome {
            ProbeOutcome::Invalidated if self.pixel_attempts < 32 => {
                self.pixel_attempts += 1;
                driver.phase = Phase::Workflows(Step::Pixels(picture, index));
            }
            ProbeOutcome::Observed(sampled, visible) if sampled > 0 && visible >= 12 => {
                self.pixel_attempts = 0;
                if !matches!(picture, Picture::Gallery | Picture::Confidence) {
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.workflow.pixels",
                            &picture.control(index),
                            picture.name(),
                            [
                                self.pixel_source as f64,
                                self.pixel_presentation as f64,
                                sampled as f64,
                                visible as f64,
                            ],
                        )
                    });
                }
                driver.phase = Phase::Workflows(match picture {
                    Picture::Confidence if index < 8 => Step::ConfidenceEdit(index + 1),
                    Picture::Confidence => Step::ConfidenceLayer(true),
                    Picture::Progress => Step::LeaveTrain,
                    Picture::Train => Step::NoImageWorkspace,
                    Picture::Validation if index < 5 => Step::Pixels(picture, index + 1),
                    Picture::Validation if self.validation_layer < 8 => Step::ValidationLayer(false, self.validation_layer + 1),
                    Picture::Validation => { self.validation_layer = 0; Step::OpenSample },
                    Picture::Detail if self.validation_layer < 8 => Step::ValidationLayer(true, self.validation_layer + 1),
                    Picture::Detail if self.validation_original < 2 => Step::ValidationOriginal,
                    Picture::Detail => Step::CloseSample,
                    Picture::Gallery => Step::Predict,
                    Picture::Compiled => Step::Source(1),
                    Picture::Image => Step::Source(2),
                    Picture::Video => Step::Restart,
                    Picture::Stop => Step::Export,
                    Picture::Theme => Step::Narrow,
                    Picture::Narrow => {
                        completed(
                            "narrow",
                            [f64::from(self.narrow_scale), 0.0, 0.0, 0.0],
                        );
                        driver.phase = Phase::Complete;
                        return;
                    }
                });
            }
            _ => driver.fail(&format!(
                "Workflow {picture:?} canvas did not contain its expected rendered content: {outcome:?}"
            )),
        }
    }
    fn retry_progress_capture(&mut self, driver: &mut Driver) {
        if self.pixel_attempts < 32 {
            self.pixel_attempts += 1;
            driver.phase = Phase::Workflows(Step::Training);
        } else {
            driver.fail("Training progress capture remained invalidated");
        }
    }
    pub(super) fn workflow_located(
        &mut self,
        driver: &mut Driver,
        control: &str,
        bounds: Rectangle,
    ) {
        let Phase::Workflows(step) = driver.phase else {
            return;
        };
        if matches!(step, Step::Pixels(Picture::Progress, _)) && self.progress_epoch.is_none() {
            self.retry_progress_capture(driver);
            return;
        }
        if matches!(step, Step::NoImageWorkspace | Step::NoValidationAspect) {
            if bounds.width > 0.0 || bounds.height > 0.0 {
                driver.fail("Workflow still exposes a removed image workspace or aspect selector");
                return;
            }
            driver.phase = Phase::Workflows(match step {
                Step::NoImageWorkspace => Step::TrainAspect,
                _ => Step::Pixels(Picture::Validation, 0),
            });
            return;
        }
        if step == Step::ChartAbsent {
            if bounds.width > 0.0 || bounds.height > 0.0 {
                driver.fail("Hidden chart remains mounted in the rendered dashboard");
            } else {
                driver.phase = Phase::Workflows(Step::ChartShow);
            }
            return;
        }
        if matches!(step, Step::TrainAspect) {
            if bounds.width <= 0.0 || bounds.height <= 0.0 {
                driver.fail("Train workspace aspect selector is missing");
            } else {
                driver.phase = Phase::Workflows(Step::ChartBounds);
            }
            return;
        }
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            driver.fail(&format!(
                "Workflow control {control} is missing from the rendered Iced tree in {step:?}"
            ));
            return;
        }
        match step {
            Step::Training | Step::Validating | Step::Predicting(_) | Step::Exporting => {
                driver
                    .reporting
                    .observe(|reporting| reporting.located(&driver.phase, control, bounds));
                return;
            }
            Step::ChartLegend | Step::ChartPan => {
                driver.phase = Phase::Workflows(if step == Step::ChartLegend {
                    Step::ChartLegendPending
                } else {
                    Step::ChartPanPending
                });
                let input =
                    crate::presentation_surface::physical_bounds(bounds, driver.input_scale);
                if !widget_ops::chart_input(Some(input), step == Step::ChartPan) {
                    driver.fail("Chart pointer dispatch failed");
                }
                return;
            }
            Step::ChartBounds | Step::ExpandedBounds => {
                self.chart_bounds = bounds;
                driver.phase = Phase::Workflows(if step == Step::ExpandedBounds {
                    Step::ExpandedChart
                } else {
                    Step::ChartTile(0)
                });
                return;
            }
            Step::ChartWheel(index, expanded) => {
                self.wheel_y = bounds.y;
                driver.phase = Phase::Workflows(Step::ChartWheelPending(index, expanded));
                if !widget_ops::chart_wheel(
                    crate::presentation_surface::physical_bounds(bounds, driver.input_scale),
                    index,
                    driver.input_scale,
                ) {
                    driver.fail("Chart wheel dispatch failed");
                }
                return;
            }
            Step::ChartScrolled(index, expanded) => {
                if bounds.y >= self.wheel_y - 0.1 {
                    driver.fail("Wheel over chart did not scroll the enclosing page");
                    return;
                }
                completed(
                    &format!(
                        "chart_wheel_{}_{}",
                        if expanded { "expanded" } else { "grid" },
                        index
                    ),
                    [self.wheel_y as f64, bounds.y as f64, index as f64, 0.0],
                );
                driver.phase = Phase::Workflows(if index < 5 {
                    Step::ChartWheel(index + 1, expanded)
                } else if expanded {
                    Step::BackToCharts
                } else {
                    Step::ExpandChart
                });
                return;
            }
            Step::ChartTile(_) | Step::ExpandedChart => {
                let index = if let Step::ChartTile(index) = step {
                    index
                } else {
                    0
                };
                let region = self.chart_bounds;
                if bounds.x < region.x - 1.0
                    || bounds.y < region.y - 1.0
                    || bounds.x + bounds.width > region.x + region.width + 1.0
                    || bounds.y + bounds.height > region.y + region.height + 1.0
                {
                    driver.fail("Training chart extends outside its workspace");
                    return;
                }
                if step == Step::ExpandedChart {
                    if (bounds.width - region.width).abs() > 2.0
                        || (bounds.height - region.height).abs() > 2.0
                    {
                        driver.fail("Expanded chart does not fill its workspace");
                        return;
                    }
                    completed(
                        "chart_expanded",
                        [bounds.width as f64, bounds.height as f64, 0.0, 0.0],
                    );
                    driver.phase = Phase::Workflows(Step::ChartSettle(Retention::Expanded));
                } else {
                    completed(
                        &format!("chart_tile_{index}"),
                        [index as f64, bounds.width as f64, bounds.height as f64, 0.0],
                    );
                    driver.phase = Phase::Workflows(if index < 5 {
                        Step::ChartTile(index + 1)
                    } else {
                        Step::ChartLegend
                    });
                }
                return;
            }
            _ => {}
        }
        driver
            .reporting
            .observe(|reporting| reporting.located(&driver.phase, control, bounds));
        // Repeated clicks on selectable checkbox text select a word. Target
        // the checkbox square so hide/show exercises the toggle itself.
        let bounds = if matches!(step, Step::ChartHide | Step::ChartShow) {
            Rectangle {
                width: bounds.width.min(bounds.height),
                ..bounds
            }
        } else {
            bounds
        };
        let bounds = match step {
            Step::Pixels(Picture::Validation, index) => atlas_cell(bounds, index),
            Step::OpenSample => atlas_cell(bounds, 0),
            Step::ValidationOriginal | Step::ValidationLayer(..) | Step::ConfidenceLayer(_) => {
                Rectangle {
                    width: bounds.width.min(bounds.height),
                    ..bounds
                }
            }
            _ => bounds,
        };
        let input = crate::presentation_surface::physical_bounds(bounds, driver.input_scale);
        if let Step::ConfidenceEdit(stage) = step {
            self.confidence_delivered = false;
            driver.phase = Phase::Workflows(Step::ConfidenceReady(stage));
            #[cfg(target_arch = "wasm32")]
            {
                let Some(mut output) = scenario_output() else {
                    driver.fail("Missing confidence input owner");
                    return;
                };
                output.receipt = None;
                let callback =
                    wasm_bindgen::closure::Closure::once_into_js(move |delivered: bool| {
                        output.send(Message::ConfidenceInputDelivered(stage, delivered));
                    });
                let action = match stage {
                    3 => 1,
                    4 => 2,
                    5 => 3,
                    _ => 0,
                };
                let value = match stage {
                    0 => "0.437",
                    1 => "",
                    2 => "2",
                    7 => "1",
                    _ => "0",
                };
                if super::confidence_input_js(
                    (input.x + input.width * 0.5) as f64,
                    (input.y + input.height * 0.5) as f64,
                    action,
                    value,
                    &callback,
                ) != 1
                {
                    driver.fail("Confidence input bridge refused delivery");
                }
            }
            return;
        }
        if let Step::Pixels(picture, index) = step {
            if matches!(
                picture,
                Picture::Validation | Picture::Detail | Picture::Confidence | Picture::Gallery
            ) && self.caption_receipt != super::probe::current_receipt(control)
            {
                return;
            }
            driver.phase = Phase::Workflows(Step::AwaitPixels(picture, index));
            #[cfg(not(target_arch = "wasm32"))]
            let _ = (input, picture.chart());
            #[cfg(target_arch = "wasm32")]
            {
                let output = scenario_output();
                let Some(mut output) = output else {
                    driver.fail("Workflow pixel callback has no live scenario owner");
                    return;
                };
                // This completion belongs to the workflow request. The browser
                // verifies its exact frame/geometry and must deliver invalidation
                // even if the observer's last generic surface receipt changed.
                output.receipt = None;
                let callback = pixel_result_callback(move |outcome| {
                    output.send(Message::WorkflowPixels {
                        picture,
                        index,
                        outcome,
                    });
                });
                workflow_pixels_js(
                    control,
                    &[
                        f64::from(input.x),
                        f64::from(input.y),
                        f64::from(input.width),
                        f64::from(input.height),
                    ],
                    picture.chart(),
                    picture == Picture::Progress,
                    self.pixel_source as f64,
                    self.pixel_presentation as f64,
                    if matches!(picture, Picture::Validation | Picture::Detail) {
                        i32::from(self.validation_layer)
                    } else {
                        -1
                    },
                    if picture == Picture::Detail {
                        6
                    } else {
                        u32::from(index)
                    },
                    &self.caption_patches,
                    &callback,
                    &self.gallery_tile,
                    &self.confidence_facts,
                );
            }
            return;
        }
        driver.phase = Phase::Workflows(match step {
            Step::ExpandChart => Step::ExpandedBounds,
            Step::BackToCharts => Step::ChartSettle(Retention::Back),
            Step::ChartSelector => Step::ChartHide,
            Step::ChartHide => Step::ChartSettle(Retention::Hidden),
            Step::ChartShow => Step::ChartCloseSelector,
            Step::ChartCloseSelector => Step::ChartSettle(Retention::Revealed),
            Step::ChartLeave => Step::ChartAway,
            Step::ChartReturn => Step::ChartSettle(Retention::Navigation),
            Step::ChartAspect(index) => Step::ChartAspectReady(index),
            Step::Train => Step::StartTrain,
            Step::StartTrain => Step::Training,
            Step::LeaveTrain => Step::HiddenTrain,
            Step::ReturnTrain => Step::Trained,
            Step::PrepareExplore => Step::OpenGallery,
            Step::OpenGallery => Step::GalleryReady,
            Step::Validate => Step::StartValidate,
            Step::StartValidate => Step::Validating,
            Step::OpenSample => Step::Sample,
            Step::ConfidenceLayer(shown) => Step::ConfidenceLayerReady(shown),
            Step::ValidationOriginal => Step::ValidationOriginalReady,
            Step::HideBoxes => Step::HiddenBoxes,
            Step::ValidationLayer(detail, index) => Step::ValidationLayerReady(detail, index),
            Step::CloseSample => Step::ClosedSample,
            Step::Explore => Step::Gallery,
            Step::Predict => Step::Source(0),
            Step::Source(index) => Step::SourceReady(index),
            Step::StartPredict(index) => Step::Predicting(index),
            Step::Pause => Step::Paused,
            Step::Resume => Step::VideoEnd,
            Step::Restart => Step::Restarted,
            Step::Stop => Step::Stopped,
            Step::Export => Step::PrepareExport,
            Step::StartExport => {
                self.export_pixels = false;
                self.primary_pixels[3] = false;
                self.primary_reveal[3] = None;
                Step::Exporting
            }
            Step::StopExport => Step::ExportStopped,
            Step::ExportReturn => Step::Pixels(Picture::Narrow, 0),
            Step::Theme => Step::Dark,
            _ => {
                driver.fail("Unexpected workflow click continuation");
                return;
            }
        });
        if !click(input) {
            driver.fail("Workflow pointer dispatch failed");
        }
    }
    pub(super) fn advance_workflows(
        &mut self,
        widgets: &mut widget_ops::RevealState,
        driver: &mut Driver,
        step: Step,
        model: &ApplicationModel,
        settings: &settings::SettingsModel,
        active: FeatureId,
        surface: Option<crate::presentation_surface::Surface>,
        router: &crate::view::router::Router,
    ) -> Task<RootMessage> {
        let settled = !settings.has_local_edits();
        let train = model.workflow.training.as_ref();
        let validation = model.workflow.validation.as_ref();
        let prediction = model.predict_snapshot.as_ref();
        let record = train.and_then(|value| value.metrics.as_ref());
        if crate::integration_control::reporting_enabled() {
            let work = match step {
                Step::Training | Step::HiddenTrain | Step::Trained => {
                    train.map(|value| (FeatureId::Train, &value.local))
                }
                Step::Validating => validation.map(|value| (FeatureId::Validate, &value.operation)),
                Step::Predicting(_) | Step::VideoEnd | Step::Restarted | Step::Stopped => {
                    prediction.map(|value| (FeatureId::Predict, &value.operation))
                }
                Step::Exporting | Step::ExportStopped => model
                    .workflow
                    .export
                    .as_ref()
                    .map(|value| (FeatureId::Export, value)),
                _ => None,
            };
            if let Some((feature, operation)) =
                work.filter(|(_, operation)| operation.active && operation.progress.sequence > 0)
            {
                let current = (
                    feature,
                    operation.generationfrontier,
                    operation.progress.sequence,
                );
                if self.work_progress != Some(current) {
                    self.work_progress = Some(current);
                    reporting::emit(|sink| {
                        sink.record(
                            "integration.workflow.operation_progress",
                            primary(feature),
                            &format!("{step:?}"),
                            [current.1 as f64, current.2 as f64, 0.0, 0.0],
                        )
                    });
                }
            }
        }
        if matches!(
            step,
            Step::Pixels(Picture::Progress, _) | Step::AwaitPixels(Picture::Progress, _)
        ) && self.progress_epoch.is_some()
            && !(train.is_some_and(|value| value.local.active)
                && record.is_some_and(|value| {
                    value.progress.phase == crate::generated::TrainingPhase::Train
                        && Some(value.progress.epoch as u64) == self.progress_epoch
                }))
        {
            // A native phase transition can remove the bar between widget
            // measurement and the asynchronous canvas read. Retire that request;
            // never interpret pixels from its old rectangle as current evidence.
            reporting::emit(|sink| {
                sink.record(
                    "integration.workflow.progress_invalidated",
                    "train.progress.bar",
                    "native phase changed before canvas completion",
                    [
                        self.progress_epoch.unwrap() as f64,
                        record.map_or(-1.0, |value| value.progress.epoch as f64),
                        0.0,
                        0.0,
                    ],
                )
            });
            self.progress_epoch = None;
        }
        let success = |value: &crate::generated::ComputeUiState| {
            !value.active && value.terminal.outcome == ComputeOperationOutcome::Succeeded
        };
        // Observe the primary through normal scrolling before progressing to
        // another card. Native progress may increase the setup column height.
        let primary_observation = match step {
            Step::Training => train.map(|value| (0, FeatureId::Train, &value.local)),
            Step::Validating => validation.map(|value| (1, FeatureId::Validate, &value.operation)),
            Step::Predicting(_) => {
                prediction.map(|value| (2, FeatureId::Predict, &value.operation))
            }
            Step::Exporting => model
                .workflow
                .export
                .as_ref()
                .map(|value| (3, FeatureId::Export, value)),
            _ => None,
        };
        if let Some((index, feature, operation)) = primary_observation
            && !self.primary_pixels[index]
            && model.primary_action_active(feature)
        {
            // A location completion immediately re-enters advance. Reveal once
            // per native progress change, then let rendering and transport run.
            let revision = (operation.generationfrontier, operation.progress.sequence);
            if !widgets.location_pending() && self.primary_reveal[index] != Some(revision) {
                self.primary_reveal[index] = Some(revision);
                return self.workflow_control(widgets, driver, primary(feature));
            }
            return Task::none();
        }
        match step {
            Step::Train | Step::ReturnTrain | Step::Theme | Step::ExportReturn => self
                .workflow_control(
                    widgets,
                    driver,
                    crate::view::navigation::stable_id(FeatureId::Train),
                ),
            Step::StartTrain
                if active == FeatureId::Train
                    && settled
                    && settings.draft.as_ref().is_some_and(|draft| {
                        model.compute_start_available(draft, FeatureId::Train)
                    }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Train))
            }
            Step::Training
                if train.is_some_and(|value| value.local.active)
                    && record.is_some_and(|value| {
                        value.progress.globaloptimizerstep > 0
                            && value.progress.phase == crate::generated::TrainingPhase::Train
                            && value.progress.completedimages > 0
                            && value.progress.completedimages < value.progress.totalimages
                    }) =>
            {
                let progress = &record.unwrap().progress;
                self.progress_epoch = Some(progress.epoch as u64);
                reporting::emit(|sink| {
                    sink.record(
                        "integration.workflow.progress",
                        "train.progress.bar",
                        "native-image-counts",
                        [
                            progress.completedimages as f64,
                            progress.totalimages as f64,
                            progress.epoch as f64,
                            progress.totalepochs as f64,
                        ],
                    )
                });
                self.workflow_step(driver, Step::Pixels(Picture::Progress, 0))
            }
            Step::LeaveTrain | Step::Validate => {
                if step == Step::LeaveTrain {
                    self.hidden_sequence = record.unwrap().sequence;
                }
                self.workflow_control(
                    widgets,
                    driver,
                    crate::view::navigation::stable_id(FeatureId::Validate),
                )
            }
            Step::HiddenTrain
                if active == FeatureId::Validate
                    && record.is_some_and(|value| value.sequence > self.hidden_sequence) =>
            {
                self.workflow_step(driver, Step::ReturnTrain)
            }
            Step::Trained
                if active == FeatureId::Train
                    && train.is_some_and(|value| success(&value.local)) =>
            {
                let Some(record) = record else {
                    driver.fail("Completed training has no metric history");
                    return Task::none();
                };
                if record.progress.globaloptimizerstep <= 1 || record.progress.val.is_none() {
                    driver.fail("Training did not publish loss and validation metrics");
                    return Task::none();
                }
                completed(
                    "train",
                    [
                        record.sequence as f64,
                        record.progress.globaloptimizerstep as f64,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(driver, Step::Pixels(Picture::Train, 0))
            }
            Step::ChartLegend
            | Step::ChartPan
            | Step::ChartLegendChanged
            | Step::ChartPanned
            | Step::ChartRetained(_)
            | Step::ChartScrolled(_, _) => {
                // Observation is effect-only and never collected in ordinary quiet runs.
                if !crate::integration_control::reporting_enabled() {
                    driver.fail("Chart retained-view acceptance requires reporting");
                    return Task::none();
                }
                let Some(view) = router.train_chart_view(crate::view::metrics::Chart::Loss) else {
                    return Task::none();
                };
                let sequence = record.map(|record| record.sequence);
                if self.chart_sequence.is_some() && self.chart_sequence != sequence {
                    driver.fail("Chart retention input overlapped a training data update");
                    return Task::none();
                }
                match step {
                    Step::ChartLegend => {
                        self.chart_sequence = sequence;
                        self.chart_view = Some(view.clone());
                        self.workflow_control(widgets, driver, view.legend_control)
                    }
                    Step::ChartLegendChanged => {
                        let previous = self.chart_view.as_ref().unwrap();
                        if previous.legend_collapsed == view.legend_collapsed
                            || previous.ranges != view.ranges
                        {
                            driver.fail("Ordinary legend input did not change only the legend");
                            return Task::none();
                        }
                        self.chart_view = Some(view);
                        completed("chart_legend", [1.0, 0.0, 0.0, 0.0]);
                        self.workflow_step(driver, Step::ChartPan)
                    }
                    Step::ChartPan => {
                        self.workflow_control(widgets, driver, "train.metrics.chart.Loss")
                    }
                    Step::ChartPanned => {
                        let previous = self.chart_view.as_ref().unwrap();
                        if previous.ranges == view.ranges
                            || previous.legend_collapsed != view.legend_collapsed
                        {
                            driver.fail("Ordinary chart pan did not change the camera");
                            return Task::none();
                        }
                        reporting::chart_view("panned", &view);
                        self.chart_view = Some(view);
                        completed("chart_pan", [1.0, 0.0, 0.0, 0.0]);
                        self.workflow_step(driver, Step::ChartWheel(0, false))
                    }
                    Step::ChartRetained(retention) => {
                        if !self.retained(driver, &view, retention.name()) {
                            return Task::none();
                        }
                        if view.visible == (retention == Retention::Hidden) {
                            driver
                                .fail("Chart visibility input did not change dashboard membership");
                            return Task::none();
                        }
                        if retention == Retention::Expanded
                            && view.plot_bounds.width
                                <= self.chart_view.as_ref().unwrap().plot_bounds.width
                        {
                            driver.fail("Expanded chart did not publish its larger plot rectangle");
                            return Task::none();
                        }
                        if retention == Retention::Navigation && active != FeatureId::Train {
                            return Task::none();
                        }
                        completed(
                            &format!("chart_retained_{}", retention.name()),
                            [
                                view.plot_bounds.width as f64,
                                view.plot_bounds.height as f64,
                                0.0,
                                0.0,
                            ],
                        );
                        let next = match retention {
                            Retention::Expanded => Step::ChartWheel(0, true),
                            Retention::Back => Step::ChartSelector,
                            Retention::Hidden => Step::ChartAbsent,
                            Retention::Revealed => Step::ChartLeave,
                            Retention::Navigation => {
                                self.chart_sequence = None;
                                Step::ChartAspect(0)
                            }
                        };
                        self.workflow_step(driver, next)
                    }
                    Step::ChartScrolled(_, _) => {
                        if !self.retained(driver, &view, "wheel") {
                            return Task::none();
                        }
                        if widgets.begin_location() {
                            locate("train.metrics.chart.Loss".into(), driver.generation)
                        } else {
                            Task::none()
                        }
                    }
                    _ => unreachable!(),
                }
            }
            Step::ChartSettle(retention) => {
                driver.phase = Phase::Workflows(Step::ChartSettlePending(retention));
                if !widget_ops::chart_input(None, false) {
                    driver.fail("Chart render settlement failed");
                }
                Task::none()
            }
            Step::ChartWheelSettle(index, expanded) => {
                driver.phase = Phase::Workflows(Step::ChartWheelSettling(index, expanded));
                if !widget_ops::chart_input(None, false) {
                    driver.fail("Chart wheel settlement failed");
                }
                Task::none()
            }
            Step::ChartWheel(_, _) => {
                self.workflow_control(widgets, driver, "train.metrics.chart.Loss")
            }
            Step::ChartSelector | Step::ChartCloseSelector => {
                self.workflow_control(widgets, driver, "train.metrics.charts")
            }
            Step::ChartAbsent => {
                if widgets.begin_location() {
                    locate("train.metrics.chart.Loss".into(), driver.generation)
                } else {
                    Task::none()
                }
            }
            Step::ChartHide | Step::ChartShow => {
                self.workflow_control(widgets, driver, "train.metrics.visible.Loss")
            }
            Step::ChartLeave => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Validate),
            ),
            Step::ChartAway if active == FeatureId::Validate => {
                self.workflow_step(driver, Step::ChartReturn)
            }
            Step::ChartReturn => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Train),
            ),
            Step::ChartBounds | Step::ExpandedBounds => {
                self.workflow_control(widgets, driver, "train.metrics.plot")
            }
            Step::ChartTile(index) => {
                let names = [
                    "Loss",
                    "Ap50",
                    "Ap",
                    "AverageRecall",
                    "Confidence",
                    "LearningRate",
                ];
                self.workflow_control(
                    widgets,
                    driver,
                    format!("train.metrics.chart.{}", names[index as usize]),
                )
            }
            Step::ExpandChart => {
                self.workflow_control(widgets, driver, "train.metrics.expand.Loss")
            }
            Step::ExpandedChart => {
                self.workflow_control(widgets, driver, "train.metrics.chart.Loss")
            }
            Step::BackToCharts => self.workflow_control(widgets, driver, "train.metrics.back"),
            Step::ChartAspect(index) => {
                let aspect = crate::generated::WORKSPACE_ASPECT_RATIO_VALUES[index as usize];
                self.workflow_control(
                    widgets,
                    driver,
                    crate::view::aspect_ratio::option_id(aspect),
                )
            }
            Step::ChartAspectReady(index) if settled => {
                let aspect = crate::generated::WORKSPACE_ASPECT_RATIO_VALUES[index as usize];
                if settings
                    .draft
                    .as_ref()
                    .is_none_or(|s| s.ui.workspaceaspectratio != aspect)
                {
                    // Pointer dispatch is asynchronous: settled old settings
                    // can precede the click. Wait for the requested value;
                    // the existing phase deadline bounds missing delivery.
                    return Task::none();
                }
                completed(
                    &format!("chart_aspect_{index}"),
                    [
                        index as f64,
                        crate::view::aspect_ratio::height_factor(aspect) as f64,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(
                    driver,
                    if index as usize + 1 < crate::generated::WORKSPACE_ASPECT_RATIO_VALUES.len() {
                        Step::ChartAspect(index + 1)
                    } else {
                        Step::PrepareExplore
                    },
                )
            }
            Step::NoImageWorkspace => widgets.arm(driver, "workflow.visual.workspace"),
            Step::TrainAspect | Step::NoValidationAspect => {
                widgets.arm(driver, "workflow.workspace.aspect")
            }
            Step::PrepareExplore => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Explore),
            ),
            Step::OpenGallery
                if active == FeatureId::Explore && settled && model.explore_open_available() =>
            {
                self.workflow_control(widgets, driver, crate::view::explore::OPEN_ID)
            }
            Step::GalleryReady if ready_gallery_tile(model).is_some() => {
                self.workflow_step(driver, Step::Validate)
            }
            Step::StartValidate
                if active == FeatureId::Validate
                    && settled
                    && settings.draft.as_ref().is_some_and(|draft| {
                        model.compute_start_available(draft, FeatureId::Validate)
                    }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Validate))
            }
            // Native metrics finish before the asynchronous sample renderer
            // necessarily publishes its final atlas. Await that same generation.
            Step::Validating
                if validation.is_some_and(|value| {
                    success(&value.operation)
                        && value.sampleavailable.iter().all(|available| *available)
                        && value.sampleidentities.iter().all(|identity| {
                            identity.generation == value.operation.generationfrontier
                        })
                }) =>
            {
                let snapshot = validation.unwrap();
                let mut identities = std::collections::BTreeSet::new();
                if snapshot.metrics.is_none()
                    || !snapshot
                        .sampleidentities
                        .iter()
                        .all(|id| identities.insert((id.generation, id.datasetindex)))
                {
                    driver.fail(
                        "Validation did not produce metrics and six distinct retained samples",
                    );
                    return Task::none();
                }
                completed(
                    "validation",
                    [
                        snapshot.operation.terminal.completed as f64,
                        identities.len() as f64,
                        snapshot.detailrows as f64,
                        0.0,
                    ],
                );
                self.confidence_metrics = snapshot.metrics.clone();
                self.confidence_generation = snapshot.operation.generationfrontier;
                if settings
                    .draft
                    .as_ref()
                    .unwrap()
                    .workflows
                    .validate
                    .display
                    .confidencethreshold
                    != 0.4
                {
                    driver.fail("Validation confidence did not start at its default");
                    return Task::none();
                }
                self.workflow_step(driver, Step::ConfidenceEdit(0)).chain(
                    iced::advanced::widget::operate(ValidationLayout::new("atlas")),
                )
            }
            Step::ConfidenceEdit(_) => self.workflow_control(
                widgets,
                driver,
                crate::generated::constraint_workflowsvalidatedisplayconfidencethreshold()
                    .stable_field_id
                    .to_string(),
            ),
            Step::ConfidenceReady(stage) if settled && self.confidence_delivered => {
                let expected = match stage {
                    0..=5 => 0.437,
                    7 => 1.0,
                    _ => 0.0,
                };
                let Some(native) = model.settings_snapshot.as_ref() else {
                    return Task::none();
                };
                if native
                    .settingsstate
                    .workflows
                    .validate
                    .display
                    .confidencethreshold
                    != expected
                    || settings
                        .draft
                        .as_ref()
                        .unwrap()
                        .workflows
                        .validate
                        .display
                        .confidencethreshold
                        != expected
                {
                    driver.fail_detail(|| {
                        format!(
                            "Validation confidence edit {stage}: expected {expected}, native {}, draft {}",
                            native.settingsstate.workflows.validate.display.confidencethreshold,
                            settings.draft.as_ref().unwrap().workflows.validate.display.confidencethreshold,
                        ).into()
                    });
                    return Task::none();
                }
                let Some(snapshot) = validation else {
                    return Task::none();
                };
                if snapshot.metrics != self.confidence_metrics
                    || snapshot.operation.generationfrontier != self.confidence_generation
                {
                    driver.fail(
                        "Display confidence changed validation results or restarted inference",
                    );
                    return Task::none();
                }
                reporting::emit(|sink| {
                    sink.record(
                        "integration.validation_confidence_edit",
                        "Display confidence",
                        "native-settled",
                        [
                            stage as f64,
                            expected as f64,
                            native.revision as f64,
                            snapshot.operation.generationfrontier as f64,
                        ],
                    )
                });
                self.workflow_step(
                    driver,
                    match stage {
                        0..=5 => Step::ConfidenceEdit(stage + 1),
                        6 => Step::ConfidenceLayer(false),
                        _ => Step::Pixels(Picture::Confidence, stage),
                    },
                )
            }
            Step::ConfidenceLayer(_) if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, "validate.gt.layer")
            }
            Step::ConfidenceLayerReady(shown)
                if validation.is_some_and(|snapshot| {
                    snapshot.overlays.groundtruthlayer == shown
                        && snapshot.overlayselection.value == snapshot.overlays
                }) =>
            {
                self.workflow_step(
                    driver,
                    if shown {
                        Step::ConfidenceLayoutResize
                    } else {
                        Step::Pixels(Picture::Confidence, 6)
                    },
                )
            }
            Step::ConfidenceLayoutResize => {
                driver.phase = Phase::Workflows(Step::ConfidenceLayoutReady);
                #[cfg(target_arch = "wasm32")]
                if !super::canvas_size_js(1200.0, 1000.0) {
                    driver.fail("Cannot resize validation for narrow controls");
                }
                Task::none()
            }
            Step::ConfidenceLayoutReady => {
                #[cfg(target_arch = "wasm32")]
                if !super::canvas_size_settled_js(1200.0, 1000.0)
                    || model.window_width != 1200
                    || model.window_height != 1000
                {
                    return Task::none();
                }
                driver.phase = Phase::Workflows(Step::ConfidenceLayoutRestore);
                iced::advanced::widget::operate(ValidationLayout::new("narrow"))
            }
            Step::ConfidenceLayoutRestore => {
                #[cfg(target_arch = "wasm32")]
                super::restore_canvas_size_js();
                self.workflow_step(driver, Step::NoValidationAspect)
            }
            Step::OpenSample if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, crate::view::validate::samples::ATLAS_ID)
            }
            Step::Sample | Step::ValidationOriginalReady
                if validation.is_some_and(|value| value.detail) =>
            {
                let Some(receipt) = super::probe::current_receipt("validate.detail.image") else {
                    return Task::none();
                };
                let Some((_, content)) =
                    crate::presentation_surface::drawable_validation(receipt.surface)
                else {
                    return Task::none();
                };
                let original = step == Step::Sample || self.validation_original == 1;
                let frame = content.frame();
                let mut expected_surface = receipt.surface;
                expected_surface.configure_original(frame, &content.metadata.frame, original);
                if receipt.surface.crop != expected_surface.crop
                    || receipt.surface.display_extent != expected_surface.display_extent
                {
                    return Task::none();
                }
                let extent = if original {
                    &frame.sourceextent
                } else {
                    &content.metadata.frame.extent
                };
                let expected = extent.width as f32 / extent.height as f32;
                if (receipt.image.width / receipt.image.height - expected).abs() > 0.01
                    || frame.sourceextent.width == frame.sourceextent.height
                    || self
                        .validation_frame
                        .is_some_and(|previous| previous.frame != receipt.surface.frame)
                {
                    driver.fail(
                        "Validation Original changed native pixels or lost source-aspect placement",
                    );
                    return Task::none();
                }
                completed(
                    "validation_original",
                    [
                        f64::from(u8::from(original)),
                        expected as f64,
                        receipt.image.width as f64,
                        receipt.image.height as f64,
                    ],
                );
                if step == Step::Sample {
                    self.validation_frame = Some(receipt.surface);
                    self.workflow_step(driver, Step::HideBoxes)
                } else {
                    self.validation_original += 1;
                    self.workflow_step(driver, Step::Pixels(Picture::Detail, 0))
                }
            }
            Step::ValidationOriginal if model.validation_navigation_available() => {
                let Some(receipt) = super::probe::current_receipt("validate.detail.image") else {
                    return Task::none();
                };
                self.validation_frame = Some(receipt.surface);
                self.workflow_control(widgets, driver, "validate.detail.original")
            }
            Step::HideBoxes if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, "validate.pred.boxes")
            }
            Step::HiddenBoxes
                if validation
                    .is_some_and(|value| value.detail && !value.overlays.predictionboxes) =>
            {
                self.workflow_step(driver, Step::Pixels(Picture::Detail, 0))
            }
            Step::ValidationLayer(_, index) if model.validation_navigation_available() => self
                .workflow_control(
                    widgets,
                    driver,
                    if index % 2 == 1 {
                        "validate.gt.layer"
                    } else {
                        "validate.pred.layer"
                    },
                ),
            Step::ValidationLayerReady(detail, index)
                if validation.is_some_and(|snapshot| {
                    snapshot.detail == detail
                        && snapshot.overlayselection.value == snapshot.overlays
                        && (
                            snapshot.overlays.groundtruthlayer,
                            snapshot.overlays.predictionlayer,
                        ) == layer_selection(index)
                }) =>
            {
                self.validation_layer = index;
                completed(
                    "validation_layer_settled",
                    [
                        f64::from(u8::from(detail)),
                        index as f64,
                        f64::from(u8::from(layer_selection(index).0)),
                        f64::from(u8::from(layer_selection(index).1)),
                    ],
                );
                self.workflow_step(
                    driver,
                    Step::Pixels(
                        if detail {
                            Picture::Detail
                        } else {
                            Picture::Validation
                        },
                        0,
                    ),
                )
            }
            Step::CloseSample if model.validation_navigation_available() => {
                self.workflow_control(widgets, driver, "validate.detail.close")
            }
            Step::ClosedSample if validation.is_some_and(|value| !value.detail) => {
                self.workflow_step(driver, Step::Explore)
            }
            Step::Explore => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Explore),
            ),
            Step::Gallery if active == FeatureId::Explore => {
                self.workflow_step(driver, Step::Pixels(Picture::Gallery, 0))
            }
            Step::Predict => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Predict),
            ),
            Step::Source(index) if active == FeatureId::Predict && settled => self
                .workflow_control(
                    widgets,
                    driver,
                    [
                        "predict.source.compiled",
                        "predict.source.image",
                        "predict.source.video",
                    ][usize::from(index)],
                ),
            Step::SourceReady(index)
                if settled
                    && settings.draft.as_ref().is_some_and(|value| {
                        value.workflows.predict.source.kind == source(index)
                    }) =>
            {
                self.generation = prediction.map_or(0, |value| value.operation.generationfrontier);
                self.workflow_step(driver, Step::StartPredict(index))
            }
            Step::StartPredict(_) | Step::Restart
                if settings.draft.as_ref().is_some_and(|draft| {
                    model.compute_start_available(draft, FeatureId::Predict)
                }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Predict))
            }
            Step::Predicting(index)
                if prediction
                    .is_some_and(|value| value.operation.generationfrontier > self.generation) =>
            {
                let snapshot = prediction.unwrap();
                if index == 2 {
                    if snapshot.operation.active && snapshot.operation.progress.completed >= 2 {
                        self.video_index = snapshot.operation.progress.completed;
                        return self.workflow_step(driver, Step::Pause);
                    }
                } else if success(&snapshot.operation) {
                    let picture = if index == 0 {
                        Picture::Compiled
                    } else {
                        Picture::Image
                    };
                    if snapshot.labels.is_empty() {
                        driver.fail("Prediction has no class labels or confidence values");
                        return Task::none();
                    }
                    completed(
                        picture.name(),
                        [
                            snapshot.operation.terminal.completed as f64,
                            snapshot.labels.len() as f64,
                            0.0,
                            0.0,
                        ],
                    );
                    return self.workflow_step(driver, Step::Pixels(picture, 0));
                }
                Task::none()
            }
            Step::Pause | Step::Resume if model.predict_pause_available() => {
                self.workflow_control(widgets, driver, "predict.pause")
            }
            Step::Paused
                if prediction.is_some_and(|value| value.paused && value.operation.active) =>
            {
                self.workflow_step(driver, Step::Resume)
            }
            Step::VideoEnd if prediction.is_some_and(|value| success(&value.operation)) => {
                let snapshot = prediction.unwrap();
                if snapshot.operation.terminal.completed <= self.video_index {
                    driver.fail("Video did not continue through EOF after resuming");
                    return Task::none();
                }
                completed(
                    "video",
                    [snapshot.operation.terminal.completed as f64, 0.0, 0.0, 0.0],
                );
                self.generation = snapshot.operation.generationfrontier;
                self.workflow_step(driver, Step::Pixels(Picture::Video, 0))
            }
            Step::Restarted
                if prediction.is_some_and(|value| {
                    value.operation.generationfrontier > self.generation
                        && value.operation.active
                        && value.operation.progress.completed > 0
                }) =>
            {
                self.workflow_step(driver, Step::Stop)
            }
            Step::Stop if model.compute_stop_available(FeatureId::Predict) => {
                self.workflow_control(widgets, driver, primary(FeatureId::Predict))
            }
            Step::Stopped
                if prediction.is_some_and(|value| {
                    !value.operation.active
                        && value.operation.terminal.outcome == ComputeOperationOutcome::Cancelled
                }) =>
            {
                completed(
                    "stop",
                    [
                        prediction.unwrap().operation.terminal.completed as f64,
                        0.0,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(driver, Step::Pixels(Picture::Stop, 0))
            }
            Step::Export => self.workflow_control(
                widgets,
                driver,
                crate::view::navigation::stable_id(FeatureId::Export),
            ),
            Step::PrepareExport
                if active == FeatureId::Export
                    && settled
                    && settings.draft.as_ref().is_some_and(|draft| {
                        model.model_selection_available(draft, FeatureId::Export)
                    }) =>
            {
                self.generation = model
                    .workflow
                    .export
                    .as_ref()
                    .map_or(0, |operation| operation.generationfrontier);
                driver.phase = Phase::Workflows(Step::StartExport);
                Task::done(RootMessage::Workspace(
                    crate::view::router::Message::Export(crate::view::export::Message::Model(
                        crate::view::workflow::model_card::Message::PrepareRequested,
                    )),
                ))
            }
            Step::StartExport
                if settings.draft.as_ref().is_some_and(|draft| {
                    model.compute_start_available(draft, FeatureId::Export)
                }) =>
            {
                self.workflow_control(widgets, driver, primary(FeatureId::Export))
            }
            Step::Exporting
                if model.workflow.export.as_ref().is_some_and(|operation| {
                    operation.generationfrontier > self.generation && operation.active
                }) =>
            {
                if self.export_pixels {
                    self.workflow_step(driver, Step::StopExport)
                } else {
                    self.workflow_control(widgets, driver, primary(FeatureId::Export))
                }
            }
            Step::StopExport if model.compute_stop_available(FeatureId::Export) => {
                self.workflow_control(widgets, driver, primary(FeatureId::Export))
            }
            Step::ExportStopped
                if model.workflow.export.as_ref().is_some_and(|operation| {
                    !operation.active
                        && operation.terminal.outcome == ComputeOperationOutcome::Cancelled
                }) =>
            {
                completed(
                    if self.export_narrow {
                        "export_stop_narrow_dark"
                    } else {
                        "export_stop"
                    },
                    [
                        model.workflow.export.as_ref().unwrap().generationfrontier as f64,
                        0.0,
                        0.0,
                        0.0,
                    ],
                );
                self.workflow_step(
                    driver,
                    if self.export_narrow {
                        Step::ExportReturn
                    } else {
                        Step::Theme
                    },
                )
            }
            Step::Dark if active == FeatureId::Train && settled => {
                driver.phase = Phase::Workflows(Step::DarkReady);
                Task::done(RootMessage::Settings(settings::Message::DarkModeChanged(
                    true,
                )))
            }
            Step::DarkReady
                if settled
                    && settings
                        .draft
                        .as_ref()
                        .is_some_and(|value| value.ui.darkmode) =>
            {
                completed("theme", [1.0, 0.0, 0.0, 0.0]);
                self.workflow_step(driver, Step::Pixels(Picture::Theme, 0))
            }
            Step::Narrow => {
                match annotation_layout_scale(driver.input_scale, model.window_width as f32, true) {
                    Ok(scale) => {
                        self.narrow_scale = scale;
                        driver.phase = Phase::Workflows(Step::NarrowReady);
                        Task::done(RootMessage::Settings(settings::Message::UiScaleChanged(
                            scale,
                        )))
                        .chain(Task::done(RootMessage::Settings(
                            settings::Message::UiScaleReleased,
                        )))
                    }
                    Err(_) => {
                        driver.fail("Workflow cannot reach its minimum-width layout");
                        Task::none()
                    }
                }
            }
            Step::NarrowReady
                if settled && (driver.input_scale - self.narrow_scale).abs() < 0.001 =>
            {
                self.export_narrow = true;
                self.workflow_step(driver, Step::Export)
            }
            Step::Pixels(picture, index) => {
                self.caption_patches.clear();
                self.caption_receipt = None;
                self.gallery_tile.clear();
                self.confidence_facts.clear();
                if picture.chart() || picture == Picture::Progress {
                    self.pixel_source = 0;
                    self.pixel_presentation = 0;
                } else {
                    let Some(surface) = surface else {
                        return Task::none();
                    };
                    let drawn = if matches!(
                        picture,
                        Picture::Validation | Picture::Detail | Picture::Confidence
                    ) {
                        crate::presentation_surface::drawable_validation(surface)
                            .filter(|(_, content)| {
                                // Native completion can precede the graphics handoff.
                                // Measure the view only when its paired image has
                                // the requested atlas/detail shape and overlay state.
                                content.metadata.detail == (picture == Picture::Detail)
                                    && (picture != Picture::Confidence
                                        || (content.metadata.display.confidencethreshold
                                            == if index == 7 { 1.0 } else { 0.0 }
                                            && !content.metadata.overlays.groundtruthlayer
                                            && content.metadata.overlays.predictionlayer))
                                    && (picture == Picture::Confidence
                                        || (
                                            content.metadata.overlays.groundtruthlayer,
                                            content.metadata.overlays.predictionlayer,
                                        ) == layer_selection(self.validation_layer))
                                    && validation.is_some_and(|snapshot| {
                                        content.metadata.overlays == snapshot.overlays
                                    })
                                    && (picture != Picture::Detail
                                        || !content.metadata.overlays.predictionboxes)
                            })
                            .and_then(|(surface, content)| {
                                let receipt =
                                    super::probe::current_receipt(&picture.control(index))?;
                                if receipt.surface.frame != surface.frame {
                                    return None;
                                }
                                if picture == Picture::Confidence {
                                    let scores: Vec<_> = content
                                        .metadata
                                        .samples
                                        .iter()
                                        .filter(|s| s.available)
                                        .flat_map(|s| &s.labels)
                                        .filter(|l| !l.groundtruth)
                                        .map(|l| l.confidence)
                                        .collect();
                                    self.confidence_facts.extend([
                                        content.metadata.display.confidencethreshold as f64,
                                        scores.len() as f64,
                                        scores.iter().copied().fold(f32::INFINITY, f32::min) as f64,
                                        scores.iter().copied().fold(f32::NEG_INFINITY, f32::max)
                                            as f64,
                                        content.metadata.frame.cleanrevision as f64,
                                        self.confidence_generation as f64,
                                        model.settings_snapshot.as_ref().unwrap().revision as f64,
                                    ]);
                                }
                                validation_caption_patches(
                                    &content,
                                    &receipt,
                                    index,
                                    &mut self.caption_patches,
                                );
                                self.caption_receipt = Some(receipt);
                                Some(surface)
                            })
                    } else if picture == Picture::Gallery {
                        ready_gallery_tile(model).map(|(receipt, tile)| {
                            let surface = receipt.surface;
                            self.gallery_tile.extend(tile);
                            self.caption_receipt = Some(receipt);
                            surface
                        })
                    } else {
                        crate::presentation_surface::drawable_prediction(surface)
                            .map(|(surface, _)| surface)
                    };
                    let Some(frame) = drawn.and_then(|value| value.frame) else {
                        return Task::none();
                    };
                    self.pixel_source = frame.content_sequence;
                    self.pixel_presentation = frame.presentation_revision;
                }
                self.workflow_control(widgets, driver, picture.control(index))
            }
            _ => Task::none(),
        }
    }
}

struct ValidationLayout {
    stage: &'static str,
    bounds: std::collections::BTreeMap<String, Rectangle>,
    labels: std::collections::BTreeSet<String>,
}
impl ValidationLayout {
    fn new(stage: &'static str) -> Self {
        Self {
            stage,
            bounds: Default::default(),
            labels: Default::default(),
        }
    }
}
impl iced::advanced::widget::Operation<RootMessage> for ValidationLayout {
    fn traverse(
        &mut self,
        operate: &mut dyn FnMut(&mut dyn iced::advanced::widget::Operation<RootMessage>),
    ) {
        operate(self);
    }
    fn container(&mut self, id: Option<&iced::advanced::widget::Id>, bounds: Rectangle) {
        for control in [
            "validate.samples.atlas",
            "validate.gt.group",
            "validate.pred.group",
        ] {
            if id == Some(&iced::advanced::widget::Id::from(control)) {
                self.bounds.insert(control.to_owned(), bounds);
            }
        }
    }
    fn text(&mut self, _id: Option<&iced::advanced::widget::Id>, _bounds: Rectangle, text: &str) {
        if [
            "Groundtruth",
            "Detections",
            "Display confidence",
            "Preview only",
        ]
        .contains(&text)
        {
            self.labels.insert(text.to_owned());
        }
    }
    fn finish(&self) -> iced::advanced::widget::operation::Outcome<RootMessage> {
        for (control, bounds) in &self.bounds {
            reporting::emit(|sink| {
                sink.record(
                    "integration.validation_layout",
                    control,
                    self.stage,
                    [
                        bounds.x as f64,
                        bounds.y as f64,
                        bounds.width as f64,
                        bounds.height as f64,
                    ],
                )
            });
        }
        for label in &self.labels {
            reporting::emit(|sink| {
                sink.record(
                    "integration.validation_text",
                    label,
                    self.stage,
                    [1.0, 0.0, 0.0, 0.0],
                )
            });
        }
        iced::advanced::widget::operation::Outcome::None
    }
}
