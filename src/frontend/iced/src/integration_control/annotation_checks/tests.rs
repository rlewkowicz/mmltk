use crate::integration_control::{Controller, CopyScaleStage, Phase};
use crate::integration_control::annotation_checks::annotation_layout_scale;
use crate::view_model::ApplicationModel;
#[test]
fn annotation_layout_sequence_supports_initially_wide_and_narrow_scales() {
    let constraint = crate::generated::constraint_uiuiscale();
    let minimum = constraint.minimum.unwrap() as f32;
    let maximum = constraint.maximum.unwrap() as f32;
    assert_eq!(maximum, 1.75);
    for (original, initially_narrow) in [(1.0, false), (maximum, true)] {
        assert!((minimum..=maximum).contains(&original));
        let physical_width = 1500.0;
        let original_width = physical_width / original;
        assert_eq!(
            original_width < crate::view::PAGE_MIN_WIDTH,
            initially_narrow
        );
        let wide = annotation_layout_scale(original, original_width, false).unwrap();
        assert!(physical_width / wide >= crate::view::PAGE_MIN_WIDTH);
        let narrow = annotation_layout_scale(wide, physical_width / wide, true).unwrap();
        assert!(physical_width / narrow < crate::view::PAGE_MIN_WIDTH);
        let mut controller = Controller::new(
            false,
            false,
            String::new(),
            String::new(),
            "512".into(),
            "copy".into(),
        );
        controller.annotation_scenario.copy_original_scale = original;
        let model = ApplicationModel::default();
        drop(controller.annotation_scenario.copy_scale_transition(&mut controller.driver, &model, narrow, CopyScaleStage::Restore));
        assert_eq!(controller.annotation_scenario.copy_requested_scale, original);
        assert_eq!(
            controller.driver.phase,
            Phase::CopyAwaitScale(CopyScaleStage::Restore)
        );
    }
}

#[test]
fn annotation_narrow_scale_obeys_native_bounds_at_packaged_dpi_widths() {
    let constraint = crate::generated::constraint_uiuiscale();
    for (unscaled_width, current_scale) in
        [(1500.0, 1.0), (1280.0, 1.5), (1500.0, 1.75), (1000.0, 1.25)]
    {
        let logical_width = unscaled_width / current_scale;
        let scale = annotation_layout_scale(current_scale, logical_width, true)
            .expect("packaged width reaches narrow layout");
        assert!(f64::from(scale) >= constraint.minimum.unwrap());
        assert!(f64::from(scale) <= constraint.maximum.unwrap());
        assert!(unscaled_width / scale < crate::view::PAGE_MIN_WIDTH);
    }
    let maximum = constraint.maximum.unwrap() as f32;
    assert!(annotation_layout_scale(maximum, 1920.0 / maximum, true).is_err());
    assert!(annotation_layout_scale(1.0, 1920.0, true).is_err());
    assert!(annotation_layout_scale(1.0, f32::NAN, true).is_err());
}

pub(in crate::integration_control) fn prepare_control_probe(controller: &mut Controller) -> bool {
    let state = &controller.annotation_scenario;
    controller.probes.prepare_control_probe(&controller.widgets, state.copy_swatch_color, state.copy_capability_available)
}

pub(in crate::integration_control) fn prepare_available_swatch(controller: &mut Controller) {
    let state = &mut controller.annotation_scenario;
    state.copy_swatch_color = [48.0, 80.0, 112.0];
    state.copy_capability_available = true;
}

pub(in crate::integration_control) fn assert_copy_completion(controller: &Controller, capability: bool, swatch: bool) {
    let state = &controller.annotation_scenario;
    assert_eq!(state.copy_capability_ready, capability);
    assert_eq!(state.copy_swatch_ready, swatch);
}
