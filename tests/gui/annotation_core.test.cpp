#include "gui/annotation_core.h"

#include "support/catch2_compat.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace mmltk::gui;

AnnotationObject make_box_object(const AnnotationBox& box) {
    AnnotationObject object;
    object.shape = AnnotationBoxShape{box};
    return object;
}

AnnotationObject make_mask_object(const AnnotationBox& box,
                                  const AnnotationMaskRegion& region,
                                  const std::vector<std::uint8_t>& mask,
                                  const std::uint64_t seed_frame_id,
                                  const std::optional<mmltk::live::LiveFrameId>& seed_live_frame_id) {
    AnnotationObject object;
    object.shape = AnnotationMaskShape{
        box,
        region,
        mask,
        seed_frame_id,
        seed_live_frame_id,
    };
    return object;
}

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

AnnotationFrame make_large_frame() {
    AnnotationFrame frame;
    frame.source_name = "large.png";
    frame.source_path = "/tmp/large.png";
    frame.frame_id = 11;
    frame.width = 32;
    frame.height = 24;
    frame.view_x = 0;
    frame.view_y = 0;
    frame.capture_width = 32;
    frame.capture_height = 24;
    frame.pixels_bgr.assign(static_cast<std::size_t>(frame.width) *
                                static_cast<std::size_t>(frame.height) * 3U,
                            192U);
    return frame;
}

fs::path make_temp_root(const char* pattern_name) {
    std::string temp_pattern =
        (fs::temp_directory_path() / (std::string(pattern_name) + ".XXXXXX")).string();
    std::vector<char> temp_buffer(temp_pattern.begin(), temp_pattern.end());
    temp_buffer.push_back('\0');
    const char* temp_root_raw = ::mkdtemp(temp_buffer.data());
    assert(temp_root_raw != nullptr);
    return temp_root_raw;
}

void write_text_file(const fs::path& path, std::string_view contents) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream stream(path, std::ios::trunc);
    assert(stream.is_open());
    stream << contents;
}

template <typename Fn>
void expect_runtime_error_contains(Fn&& fn, const std::string& needle) {
    bool threw = false;
    try {
        std::forward<Fn>(fn)();
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find(needle) != std::string::npos);
    }
    assert(threw);
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

    AnnotationObject instance = make_box_object(AnnotationBox{0, 0, 4, 2});
    instance.category_index = 0;
    recenter_annotation_range(instance.sup, AnnotationHsv{0.0f, 1.0f, 1.0f});
    instance.sup.tolerance.hue_minus_pct = 1.0f;
    instance.sup.tolerance.hue_plus_pct = 1.0f;
    instance.sup.tolerance.saturation_minus_pct = 1.0f;
    instance.sup.tolerance.saturation_plus_pct = 1.0f;
    instance.sup.tolerance.value_minus_pct = 1.0f;
    instance.sup.tolerance.value_plus_pct = 1.0f;

    const AnnotationPreviewResult preview =
        build_annotation_preview(make_frame(), categories, {instance}, false);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
    assert(resolved.bbox.x1 == 2);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 4);
    assert(resolved.bbox.y2 == 2);
    assert(resolved.mask_rle == "2:2 6:2");
}

void test_preview_nosup_restores_suppressed_pixels() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationObject instance = make_box_object(AnnotationBox{0, 0, 2, 2});
    instance.category_index = 0;
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
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 2);
    assert(resolved.bbox.y2 == 2);
    assert(resolved.mask_rle == "0:2 4:2");
}

void test_resolved_crop_preserves_mask_alpha() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationObject instance = make_box_object(AnnotationBox{0, 0, 3, 3});
    instance.category_index = 0;
    recenter_annotation_range(instance.sup, AnnotationHsv{0.0f, 1.0f, 1.0f});
    instance.sup.tolerance.hue_minus_pct = 1.0f;
    instance.sup.tolerance.hue_plus_pct = 1.0f;
    instance.sup.tolerance.saturation_minus_pct = 1.0f;
    instance.sup.tolerance.saturation_plus_pct = 1.0f;
    instance.sup.tolerance.value_minus_pct = 1.0f;
    instance.sup.tolerance.value_plus_pct = 1.0f;

    const AnnotationPreviewResult preview =
        build_annotation_preview(make_frame(), categories, {instance}, false);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
    assert(resolved.crop_width == 3U);
    assert(resolved.crop_height == 3U);
    assert(resolved.crop_rgba.size() == (static_cast<size_t>(3U) * 3U * 4U));
    assert(resolved.crop_rgba[3] == 0U);
    assert(resolved.crop_rgba[7] == 0U);
    assert(resolved.crop_rgba[15] == 0U);
    assert(resolved.crop_rgba[19] == 0U);
    assert(resolved.crop_rgba[11] == 255U);
    assert(resolved.crop_rgba[23] == 255U);
    assert(resolved.crop_rgba[35] == 255U);
}

void test_prediction_mask_decode_and_bbox() {
    mmltk::rfdetr::EncodedMask encoded;
    encoded.width = 4;
    encoded.height = 4;
    encoded.runs = {{5U, 2U}, {9U, 1U}};
    const std::vector<std::uint8_t> dense = decode_annotation_prediction_mask(encoded, 4, 4);
    const std::optional<AnnotationBox> bbox = annotation_bbox_from_mask(dense, 4, 4);
    if (!bbox.has_value()) {
        throw std::runtime_error("expected decoded prediction mask bbox");
    }
    const AnnotationBox bbox_value = bbox.value();
    assert(bbox_value.x1 == 1);
    assert(bbox_value.y1 == 1);
    assert(bbox_value.x2 == 3);
    assert(bbox_value.y2 == 3);
    assert(encode_annotation_mask_rle(dense) == "5:2 9:1");
}

void test_mask_rle_round_trip_decode() {
    const std::vector<std::uint8_t> mask = {
        0, 1, 1, 0,
        0, 0, 1, 0,
        1, 1, 0, 0,
        0, 0, 0, 1,
    };
    const std::string encoded = encode_annotation_mask_rle(mask);
    const std::vector<std::uint8_t> decoded = decode_annotation_mask_rle(encoded, 4, 4);
    assert(decoded == mask);
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

    AnnotationObject instance = make_box_object(AnnotationBox{2, 0, 4, 2});
    instance.category_index = 0;
    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, false);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
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

    std::vector<std::uint8_t> seed_mask(16U, 0U);
    seed_mask[5] = 1U;
    AnnotationObject instance = make_mask_object(AnnotationBox{1, 1, 3, 3},
                                                 AnnotationMaskRegion{0, 0, 4, 4},
                                                 seed_mask,
                                                 cropped.frame_id,
                                                 std::nullopt);
    instance.category_index = 0;

    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, false);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
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

    std::vector<std::uint8_t> seed_mask(16U, 0U);
    seed_mask[5] = 1U;
    AnnotationObject instance = make_mask_object(AnnotationBox{1, 1, 3, 3},
                                                 AnnotationMaskRegion{0, 0, 4, 4},
                                                 seed_mask,
                                                 0U,
                                                 std::nullopt);
    instance.category_index = 0;

    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, true);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 1);
    assert(resolved.bbox.y2 == 1);
    assert(resolved.mask_rle == "0:1");
}

void test_live_model_mask_requires_matching_live_frame_identity() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationFrame cropped = extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 3, 3});
    cropped.live_frame_id = mmltk::live::LiveFrameId{41U, cropped.frame_id};

    std::vector<std::uint8_t> seed_mask(16U, 0U);
    seed_mask[5] = 1U;
    AnnotationObject instance = make_mask_object(AnnotationBox{1, 1, 3, 3},
                                                 AnnotationMaskRegion{0, 0, 4, 4},
                                                 seed_mask,
                                                 cropped.frame_id,
                                                 mmltk::live::LiveFrameId{99U, cropped.frame_id});
    instance.category_index = 0;

    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, true);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
    assert(resolved.bbox.x1 == 0);
    assert(resolved.bbox.y1 == 0);
    assert(resolved.bbox.x2 == 2);
    assert(resolved.bbox.y2 == 2);
    assert(resolved.mask_rle == "0:4");
}

void test_live_model_mask_uses_matching_live_frame_identity_even_if_display_id_drifts() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");

    AnnotationFrame cropped = extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 3, 3});
    cropped.live_frame_id = mmltk::live::LiveFrameId{41U, cropped.frame_id};

    std::vector<std::uint8_t> seed_mask(16U, 0U);
    seed_mask[5] = 1U;
    AnnotationObject instance = make_mask_object(AnnotationBox{1, 1, 3, 3},
                                                 AnnotationMaskRegion{0, 0, 4, 4},
                                                 seed_mask,
                                                 cropped.frame_id + 100U,
                                                 cropped.live_frame_id);
    instance.category_index = 0;

    const AnnotationPreviewResult preview =
        build_annotation_preview(cropped, categories, {instance}, true);
    assert(preview.resolved_objects.size() == 1U);
    const AnnotationResolvedObject& resolved = preview.resolved_objects.front();
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

    AnnotationObject instance = make_box_object(AnnotationBox{2, 0, 4, 2});
    instance.category_index = 0;
    const fs::path temp_root = make_temp_root("mmltk-test-gui-annotation-core");

    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = "train";
    const AnnotationSaveResult save =
        save_annotation_scene(config, frame, categories, {instance}, false);

    assert(fs::exists(save.scene_image_path));
    assert(fs::exists(save.scene_jsonl_path));
    assert(save.entity_paths.size() == 1U);
    assert(fs::exists(save.entity_paths.front()));
    assert(fs::exists(temp_root / "categories.json"));
    assert(fs::exists(temp_root / "manifests" / "scenes.jsonl"));
    assert(fs::exists(temp_root / "manifests" / "entities.jsonl"));
    AnnotationCategories loaded_categories = load_annotation_categories(temp_root);
    const std::optional<std::vector<AnnotationObject>> loaded_objects =
        load_saved_annotation_scene_for_frame(temp_root, frame, &loaded_categories);
    if (!loaded_objects.has_value()) {
        throw std::runtime_error("expected saved annotation scene to reload");
    }
    const auto& loaded_objects_value = loaded_objects.value();
    assert(loaded_objects_value.size() == 1U);
    const AnnotationBoxShape* loaded_box =
        std::get_if<AnnotationBoxShape>(&loaded_objects_value.front().shape);
    assert(loaded_box != nullptr);
    assert(loaded_box->box.x1 == 2);
    assert(loaded_box->box.y1 == 0);
    assert(loaded_box->box.x2 == 4);
    assert(loaded_box->box.y2 == 2);

}

void test_scene_round_trip_preserves_point_capture_space() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "point");
    const AnnotationFrame cropped =
        extract_annotation_frame_region(make_large_frame(), AnnotationBox{4, 3, 20, 18});

    AnnotationObject point;
    point.object_id = "manual-1";
    point.category_index = 0U;
    point.shape = AnnotationPointShape{AnnotationPoint{7.0f, 9.0f}};

    const fs::path temp_root = make_temp_root("mmltk-test-gui-annotation-point");
    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = "train";
    const AnnotationSaveResult save =
        save_annotation_scene(config, cropped, categories, {point}, false);

    AnnotationCategories loaded_categories = load_annotation_categories(temp_root);
    const std::vector<AnnotationObject> loaded_objects =
        load_annotation_scene_objects(save.scene_jsonl_path, &loaded_categories);
    assert(loaded_objects.size() == 1U);
    assert(loaded_categories.items.size() == 1U);
    assert(loaded_categories.items.front().name == "point");

    const AnnotationPointShape* loaded_point =
        std::get_if<AnnotationPointShape>(&loaded_objects.front().shape);
    assert(loaded_point != nullptr);
    assert(loaded_point->point.x == 7.0f);
    assert(loaded_point->point.y == 9.0f);
}

void test_load_annotation_categories_rejects_missing_required_schema_fields() {
    const fs::path temp_root = make_temp_root("mmltk-test-gui-annotation-categories");
    const fs::path categories_path = temp_root / "categories.json";

    write_text_file(categories_path, R"({"classes":[]})");
    expect_runtime_error_contains(
        [&]() { (void)load_annotation_categories(temp_root); },
        "missing object `meta`");

    write_text_file(categories_path,
                    R"({
  "meta": {
    "dataset_name": "sample",
    "version": "3.0",
    "image_format": "png",
    "bbox_format": "xyxy_absolute_pixels",
    "mask_format": "rle_row_major_start_length",
    "shape_types": ["box", "mask", "spline", "point", "skeleton"],
    "background_annotation_policy": "empty_jsonl_file"
  }
})");
    expect_runtime_error_contains(
        [&]() { (void)load_annotation_categories(temp_root); },
        "missing array `classes`");
}

void test_load_annotation_categories_rejects_wrong_schema_version() {
    const fs::path temp_root = make_temp_root("mmltk-test-gui-annotation-version");
    write_text_file(temp_root / "categories.json",
                    R"({
  "meta": {
    "dataset_name": "sample",
    "version": "2.0",
    "image_format": "png",
    "bbox_format": "xyxy_absolute_pixels",
    "mask_format": "rle_row_major_start_length",
    "shape_types": ["box", "mask", "spline", "point", "skeleton"],
    "background_annotation_policy": "empty_jsonl_file"
  },
  "classes": []
})");
    expect_runtime_error_contains(
        [&]() { (void)load_annotation_categories(temp_root); },
        "unexpected `version`");
}

void test_load_annotation_scene_objects_rejects_malformed_shape_records() {
    const fs::path temp_root = make_temp_root("mmltk-test-gui-scene-errors");
    const fs::path scene_path = temp_root / "scene.jsonl";

    write_text_file(scene_path,
                    R"({"class":"point","shape_type":"point","image_size_wh":[32,24],"view_origin_xy":[0,0],"capture_size_wh":[32,24],"shape":{}}
)");
    expect_runtime_error_contains(
        [&]() { (void)load_annotation_scene_objects(scene_path, nullptr); },
        "point record is missing `shape.xy`");

    write_text_file(scene_path,
                    R"({"class":"curve","shape_type":"spline","image_size_wh":[32,24],"view_origin_xy":[0,0],"capture_size_wh":[32,24],"shape":{"closed":false}}
)");
    expect_runtime_error_contains(
        [&]() { (void)load_annotation_scene_objects(scene_path, nullptr); },
        "spline record is missing array `shape.knots`");

    write_text_file(scene_path,
                    R"({"class":"pose","shape_type":"skeleton","image_size_wh":[32,24],"view_origin_xy":[0,0],"capture_size_wh":[32,24],"shape":{"nodes":[]}}
)");
    expect_runtime_error_contains(
        [&]() { (void)load_annotation_scene_objects(scene_path, nullptr); },
        "skeleton record is missing array `shape.edges`");
}

void test_scene_round_trip_preserves_spline_topology() {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "curve");
    const AnnotationFrame cropped =
        extract_annotation_frame_region(make_large_frame(), AnnotationBox{4, 3, 20, 18});

    AnnotationObject spline;
    spline.object_id = "manual-1";
    spline.category_index = 0U;
    spline.shape = AnnotationSplineShape{
        true,
        {
            AnnotationSplineKnot{
                AnnotationPoint{6.0f, 5.0f},
                AnnotationSplineHandle{AnnotationPoint{5.0f, 4.0f}, true},
                AnnotationSplineHandle{AnnotationPoint{9.0f, 5.5f}, true},
                AnnotationSplineHandleMode::Smooth,
            },
            AnnotationSplineKnot{
                AnnotationPoint{14.0f, 11.0f},
                AnnotationSplineHandle{AnnotationPoint{12.0f, 9.0f}, true},
                AnnotationSplineHandle{AnnotationPoint{16.0f, 12.0f}, true},
                AnnotationSplineHandleMode::Mirrored,
            },
        },
    };

    const fs::path temp_root = make_temp_root("mmltk-test-gui-annotation-spline");
    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = "train";
    const AnnotationSaveResult save =
        save_annotation_scene(config, cropped, categories, {spline}, false);

    AnnotationCategories loaded_categories = load_annotation_categories(temp_root);
    const std::vector<AnnotationObject> loaded_objects =
        load_annotation_scene_objects(save.scene_jsonl_path, &loaded_categories);
    assert(loaded_objects.size() == 1U);
    assert(loaded_categories.items.size() == 1U);
    assert(loaded_categories.items.front().name == "curve");

    const AnnotationSplineShape* loaded_spline =
        std::get_if<AnnotationSplineShape>(&loaded_objects.front().shape);
    assert(loaded_spline != nullptr);
    assert(loaded_spline->closed);
    assert(loaded_spline->knots.size() == 2U);
    assert(loaded_spline->knots[0].position.x == 6.0f);
    assert(loaded_spline->knots[0].position.y == 5.0f);
    assert(loaded_spline->knots[0].in_handle.enabled);
    assert(loaded_spline->knots[0].in_handle.position.x == 5.0f);
    assert(loaded_spline->knots[0].in_handle.position.y == 4.0f);
    assert(loaded_spline->knots[0].out_handle.enabled);
    assert(loaded_spline->knots[0].out_handle.position.x == 9.0f);
    assert(loaded_spline->knots[0].out_handle.position.y == 5.5f);
    assert(loaded_spline->knots[0].handle_mode == AnnotationSplineHandleMode::Smooth);
    assert(loaded_spline->knots[1].position.x == 14.0f);
    assert(loaded_spline->knots[1].position.y == 11.0f);
    assert(loaded_spline->knots[1].in_handle.enabled);
    assert(loaded_spline->knots[1].out_handle.enabled);
    assert(loaded_spline->knots[1].handle_mode == AnnotationSplineHandleMode::Mirrored);
}

void test_scene_round_trip_preserves_skeleton_topology() {
    AnnotationCategories categories;
    const std::size_t category_index = ensure_annotation_category(categories, "pose");
    categories.items[category_index].keypoints = {"left", "right", "tail"};
    categories.items[category_index].skeleton_edges = {
        AnnotationCategorySkeletonEdge{0U, 1U},
        AnnotationCategorySkeletonEdge{1U, 2U},
    };

    AnnotationObject skeleton;
    skeleton.object_id = "manual-1";
    skeleton.category_index = category_index;
    skeleton.shape = AnnotationSkeletonShape{
        {
            AnnotationSkeletonNode{"left", AnnotationPoint{3.0f, 4.0f}, true},
            AnnotationSkeletonNode{"right", AnnotationPoint{8.0f, 6.0f}, true},
            AnnotationSkeletonNode{"tail", AnnotationPoint{12.0f, 9.0f}, false},
        },
        {
            AnnotationSkeletonEdge{0U, 1U},
            AnnotationSkeletonEdge{1U, 2U},
        },
    };

    const fs::path temp_root = make_temp_root("mmltk-test-gui-annotation-skeleton");
    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = "val";
    const AnnotationSaveResult save =
        save_annotation_scene(config, make_large_frame(), categories, {skeleton}, false);

    AnnotationCategories loaded_categories = load_annotation_categories(temp_root);
    const std::vector<AnnotationObject> loaded_objects =
        load_annotation_scene_objects(save.scene_jsonl_path, &loaded_categories);
    assert(loaded_objects.size() == 1U);

    const AnnotationSkeletonShape* loaded_skeleton =
        std::get_if<AnnotationSkeletonShape>(&loaded_objects.front().shape);
    assert(loaded_skeleton != nullptr);
    assert(loaded_skeleton->nodes.size() == 3U);
    assert(loaded_skeleton->edges.size() == 2U);
    assert(loaded_skeleton->nodes[0].key == "left");
    assert(loaded_skeleton->nodes[0].visible);
    assert(loaded_skeleton->nodes[2].key == "tail");
    assert(!loaded_skeleton->nodes[2].visible);
    assert(loaded_skeleton->edges[1].source_index == 1U);
    assert(loaded_skeleton->edges[1].target_index == 2U);
    assert(loaded_categories.items.size() == 1U);
    assert(loaded_categories.items[0].keypoints.size() == 3U);
    assert(loaded_categories.items[0].skeleton_edges.size() == 2U);
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_recenter_resets_tolerances);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_preview_builds_mask_from_box_minus_sup);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_preview_nosup_restores_suppressed_pixels);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_resolved_crop_preserves_mask_alpha);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_prediction_mask_decode_and_bbox);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_mask_rle_round_trip_decode);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_capture_space_box_projects_into_cropped_frame);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_capture_space_model_mask_projects_into_cropped_frame);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_persistent_capture_space_model_mask_survives_live_mode);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_live_model_mask_requires_matching_live_frame_identity);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]",
                         test_live_model_mask_uses_matching_live_frame_identity_even_if_display_id_drifts);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_box_round_trip_between_capture_and_frame_space);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_save_scene_writes_outputs);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_scene_round_trip_preserves_point_capture_space);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_load_annotation_categories_rejects_missing_required_schema_fields);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_load_annotation_categories_rejects_wrong_schema_version);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_load_annotation_scene_objects_rejects_malformed_shape_records);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_scene_round_trip_preserves_spline_topology);
MMLTK_REGISTER_TEST_CASE("[gui][annotation_core]", test_scene_round_trip_preserves_skeleton_topology);
