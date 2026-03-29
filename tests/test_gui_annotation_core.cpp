#include "gui/annotation_core.h"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace fastloader::gui;

AnnotationFrame make_frame() {
    AnnotationFrame frame;
    frame.source_name = "sample.png";
    frame.source_path = "/tmp/sample.png";
    frame.frame_id = 7;
    frame.width = 4;
    frame.height = 4;
    frame.view_x = 0;
    frame.view_y = 0;
    frame.capture_width = 4;
    frame.capture_height = 4;
    frame.pixels_bgr = {
        0, 0, 255,   0, 0, 255,   0, 255, 0,   0, 255, 0,
        0, 0, 255,   0, 0, 255,   0, 255, 0,   0, 255, 0,
        255, 0, 0,   255, 0, 0,   255, 255, 255, 255, 255, 255,
        255, 0, 0,   255, 0, 0,   255, 255, 255, 255, 255, 255,
    };
    return frame;
}

void test_recenter_resets_tolerances() {
    AnnotationColorRange range;
    range.tolerance.hue_minus_pct = 12.0f;
    range.tolerance.value_plus_pct = 18.0f;
    range.sampling = true;
    recenter_annotation_range(range, AnnotationHsv{30.0f, 0.75f, 0.25f});
    assert(range.center.hue_degrees == 30.0f);
    assert(range.center.saturation == 0.75f);
    assert(range.center.value == 0.25f);
    assert(!annotation_range_active(range));
    assert(!range.sampling);
}

void test_preview_builds_mask_from_box_minus_sup() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.box = AnnotationBox{0, 0, 4, 2};
    recenter_annotation_range(instance.sup, AnnotationHsv{0.0f, 1.0f, 1.0f});
    instance.sup.tolerance.hue_minus_pct = 1.0f;
    instance.sup.tolerance.hue_plus_pct = 1.0f;
    instance.sup.tolerance.saturation_minus_pct = 1.0f;
    instance.sup.tolerance.saturation_plus_pct = 1.0f;
    instance.sup.tolerance.value_minus_pct = 1.0f;
    instance.sup.tolerance.value_plus_pct = 1.0f;

    const AnnotationPreviewResult preview =
        build_annotation_preview(make_frame(), categories, {instance}, false);
    assert(preview.resolved_instances.size() == 1U);
    const AnnotationResolvedInstance& resolved = preview.resolved_instances.front();
    assert(resolved.bbox.x1 == 2);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 4);
    assert(resolved.bbox.y2 == 2);
    assert(resolved.mask_rle == "2:2 6:2");
}

void test_preview_nosup_restores_suppressed_pixels() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.box = AnnotationBox{0, 0, 2, 2};
    recenter_annotation_range(instance.sup, AnnotationHsv{0.0f, 1.0f, 1.0f});
    recenter_annotation_range(instance.nosup, AnnotationHsv{0.0f, 1.0f, 1.0f});
    for (float* value : {
             &instance.sup.tolerance.hue_minus_pct,
             &instance.sup.tolerance.hue_plus_pct,
             &instance.sup.tolerance.saturation_minus_pct,
             &instance.sup.tolerance.saturation_plus_pct,
             &instance.sup.tolerance.value_minus_pct,
             &instance.sup.tolerance.value_plus_pct,
             &instance.nosup.tolerance.hue_minus_pct,
             &instance.nosup.tolerance.hue_plus_pct,
             &instance.nosup.tolerance.saturation_minus_pct,
             &instance.nosup.tolerance.saturation_plus_pct,
             &instance.nosup.tolerance.value_minus_pct,
             &instance.nosup.tolerance.value_plus_pct,
         }) {
        *value = 1.0f;
    }

    const AnnotationPreviewResult preview =
        build_annotation_preview(make_frame(), categories, {instance}, false);
    assert(preview.resolved_instances.size() == 1U);
    const AnnotationResolvedInstance& resolved = preview.resolved_instances.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 2);
    assert(resolved.bbox.y2 == 2);
    assert(resolved.mask_rle == "0:2 4:2");
}

void test_resolved_crop_preserves_mask_alpha() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.box = AnnotationBox{0, 0, 3, 3};
    recenter_annotation_range(instance.sup, AnnotationHsv{0.0f, 1.0f, 1.0f});
    instance.sup.tolerance.hue_minus_pct = 1.0f;
    instance.sup.tolerance.hue_plus_pct = 1.0f;
    instance.sup.tolerance.saturation_minus_pct = 1.0f;
    instance.sup.tolerance.saturation_plus_pct = 1.0f;
    instance.sup.tolerance.value_minus_pct = 1.0f;
    instance.sup.tolerance.value_plus_pct = 1.0f;

    const AnnotationPreviewResult preview =
        build_annotation_preview(make_frame(), categories, {instance}, false);
    assert(preview.resolved_instances.size() == 1U);
    const AnnotationResolvedInstance& resolved = preview.resolved_instances.front();
    assert(resolved.crop_width == 3U);
    assert(resolved.crop_height == 3U);
    assert(resolved.crop_rgba.size() == 3U * 3U * 4U);
    assert(resolved.crop_rgba[3] == 0U);
    assert(resolved.crop_rgba[7] == 0U);
    assert(resolved.crop_rgba[15] == 0U);
    assert(resolved.crop_rgba[19] == 0U);
    assert(resolved.crop_rgba[11] == 255U);
    assert(resolved.crop_rgba[23] == 255U);
    assert(resolved.crop_rgba[35] == 255U);
}

void test_prediction_mask_decode_and_bbox() {
    fastloader::rfdetr::EncodedMask encoded;
    encoded.width = 4;
    encoded.height = 4;
    encoded.runs = {{5U, 2U}, {9U, 1U}};
    const std::vector<std::uint8_t> dense = decode_annotation_prediction_mask(encoded, 4, 4);
    const std::optional<AnnotationBox> bbox = annotation_bbox_from_mask(dense, 4, 4);
    assert(bbox.has_value());
    assert(bbox->x1 == 1);
    assert(bbox->y1 == 1);
    assert(bbox->x2 == 3);
    assert(bbox->y2 == 3);
    assert(encode_annotation_mask_rle(dense) == "5:2 9:1");
}

void test_capture_space_box_projects_into_cropped_frame() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    const AnnotationFrame cropped = extract_annotation_frame_region(make_frame(), AnnotationBox{2, 0, 4, 2});
    assert(cropped.width == 2U);
    assert(cropped.height == 2U);
    assert(cropped.view_x == 2U);
    assert(cropped.view_y == 0U);
    assert(cropped.capture_width == 4U);
    assert(cropped.capture_height == 4U);

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.box = AnnotationBox{2, 0, 4, 2};
    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, false);
    assert(preview.resolved_instances.size() == 1U);
    const AnnotationResolvedInstance& resolved = preview.resolved_instances.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 2);
    assert(resolved.bbox.y2 == 2);
    assert(resolved.mask_rle == "0:4");
}

void test_capture_space_model_mask_projects_into_cropped_frame() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    const AnnotationFrame cropped = extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 3, 3});

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.seed_kind = AnnotationSeedKind::ModelMask;
    instance.box = AnnotationBox{1, 1, 3, 3};
    instance.seed_frame_id = cropped.frame_id;
    instance.seed_mask_region = AnnotationMaskRegion{0, 0, 4, 4};
    instance.seed_mask.assign(16U, 0U);
    instance.seed_mask[5] = 1U;

    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, false);
    assert(preview.resolved_instances.size() == 1U);
    const AnnotationResolvedInstance& resolved = preview.resolved_instances.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 1);
    assert(resolved.bbox.y2 == 1);
    assert(resolved.mask_rle == "0:1");
}

void test_persistent_capture_space_model_mask_survives_live_mode() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    const AnnotationFrame cropped = extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 3, 3});

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.seed_kind = AnnotationSeedKind::ModelMask;
    instance.box = AnnotationBox{1, 1, 3, 3};
    instance.seed_frame_id = 0U;
    instance.seed_mask_region = AnnotationMaskRegion{0, 0, 4, 4};
    instance.seed_mask.assign(16U, 0U);
    instance.seed_mask[5] = 1U;

    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, true);
    assert(preview.resolved_instances.size() == 1U);
    const AnnotationResolvedInstance& resolved = preview.resolved_instances.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 1);
    assert(resolved.bbox.y2 == 1);
    assert(resolved.mask_rle == "0:1");
}

void test_box_round_trip_between_capture_and_frame_space() {
    AnnotationFrame frame = extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 4, 4});
    const AnnotationBox capture_box{2, 2, 4, 4};
    const AnnotationBox frame_box = annotation_box_to_frame(frame, capture_box);
    assert(frame_box.x1 == 1);
    assert(frame_box.y1 == 1);
    assert(frame_box.x2 == 3);
    assert(frame_box.y2 == 3);
    const AnnotationBox round_trip = annotation_box_from_frame(frame, frame_box);
    assert(round_trip.x1 == capture_box.x1);
    assert(round_trip.y1 == capture_box.y1);
    assert(round_trip.x2 == capture_box.x2);
    assert(round_trip.y2 == capture_box.y2);
}

void test_save_scene_writes_outputs() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");
    AnnotationFrame frame = make_frame();

    AnnotationInstance instance;
    instance.category_index = 0;
    instance.box = AnnotationBox{2, 0, 4, 2};
    const AnnotationPreviewResult preview =
        build_annotation_preview(frame, categories, {instance}, false);
    assert(preview.resolved_instances.size() == 1U);

    const fs::path temp_root =
        fs::temp_directory_path() / "fastloader-test-gui-annotation-core";
    fs::remove_all(temp_root);

    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = "train";
    const AnnotationSaveResult save =
        save_annotation_scene(config, frame, categories, preview.resolved_instances);

    assert(fs::exists(save.scene_image_path));
    assert(fs::exists(save.scene_jsonl_path));
    assert(save.entity_paths.size() == 1U);
    assert(fs::exists(save.entity_paths.front()));
    assert(fs::exists(temp_root / "categories.json"));
    assert(fs::exists(temp_root / "manifests" / "scenes.jsonl"));
    assert(fs::exists(temp_root / "manifests" / "entities.jsonl"));

    fs::remove_all(temp_root);
}

} // namespace

int main() {
    test_recenter_resets_tolerances();
    test_preview_builds_mask_from_box_minus_sup();
    test_preview_nosup_restores_suppressed_pixels();
    test_resolved_crop_preserves_mask_alpha();
    test_prediction_mask_decode_and_bbox();
    test_capture_space_box_projects_into_cropped_frame();
    test_capture_space_model_mask_projects_into_cropped_frame();
    test_persistent_capture_space_model_mask_survives_live_mode();
    test_box_round_trip_between_capture_and_frame_space();
    test_save_scene_writes_outputs();
    return 0;
}
