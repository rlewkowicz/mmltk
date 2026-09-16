#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/filesystem_test_utils.hpp"
import mmltk.backend.imaging.annotation.core;
namespace fs = std::filesystem;
namespace {
using namespace mmltk::backend::imaging::annotation;
using mmltk::testsupport::ScopedTempDir;
using mmltk::testsupport::write_text_file;
constexpr AnnotationHsv kRedMaskHsv{0.0f, 1.0f, 1.0f};
template <typename Fn>
void expect_runtime_error_contains(Fn&& operation, const std::string_view expected) {
    bool threw = false;
    try {
        std::forward<Fn>(operation)();
    } catch (const std::runtime_error& error) {
        threw = true;
        REQUIRE(std::string_view(error.what()).find(expected) != std::string_view::npos);
    }
    REQUIRE(threw);
}
AnnotationObject make_box_object(const AnnotationBox& box) {
    AnnotationObject object;
    object.shape = AnnotationBoxShape{box};
    return object;
}
AnnotationObject make_mask_object(const AnnotationBox& box, const AnnotationMaskRegion& region, const std::vector<std::uint8_t>& mask,
                                  const std::uint64_t seed_frame_id, const std::optional<ContentIdentity>& seed_live_frame_id) {
    AnnotationObject object;
    object.shape = AnnotationMaskShape{
        box, region, mask, seed_frame_id, seed_live_frame_id, nullptr, {},
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
    set_annotation_frame_pixels(frame, {
                                           0,   0, 255, 0,   0, 255, 0,   255, 0,   0,   255, 0,   0,   0, 255, 0,   0, 255, 0,   255, 0,   0,   255, 0,
                                           255, 0, 0,   255, 0, 0,   255, 255, 255, 255, 255, 255, 255, 0, 0,   255, 0, 0,   255, 255, 255, 255, 255, 255,
                                       });
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
    set_annotation_frame_pixels(frame, std::vector<std::uint8_t>(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height) * 3U, 192U));
    return frame;
}
AnnotationFrame make_capture_space_frame() { return extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 3, 3}); }
AnnotationCategories make_single_category(const char* category_name) {
    AnnotationCategories categories;
    ensure_annotation_category(categories, category_name);
    return categories;
}
void set_unit_tolerance(AnnotationColorRange& range) {
    for (float* value : {
             &range.tolerance.hue_minus_pct,
             &range.tolerance.hue_plus_pct,
             &range.tolerance.saturation_minus_pct,
             &range.tolerance.saturation_plus_pct,
             &range.tolerance.value_minus_pct,
             &range.tolerance.value_plus_pct,
         }) {
        *value = 1.0f;
    }
}
void recenter_with_unit_tolerance(AnnotationColorRange& range, const AnnotationHsv& center) {
    recenter_annotation_range(range, center);
    set_unit_tolerance(range);
}
AnnotationObject make_reticle_box_preview_object(const AnnotationBox& box, const bool use_sup = false, const bool use_nosup = false) {
    AnnotationObject object = make_box_object(box);
    object.category_index = 0U;
    if (use_sup) { recenter_with_unit_tolerance(object.sup, kRedMaskHsv); }
    if (use_nosup) { recenter_with_unit_tolerance(object.nosup, kRedMaskHsv); }
    return object;
}
AnnotationFrame make_cropped_frame(const AnnotationBox& crop_box) { return extract_annotation_frame_region(make_frame(), crop_box); }
std::vector<std::uint8_t> make_single_pixel_seed_mask() {
    std::vector<std::uint8_t> seed_mask(16U, 0U);
    seed_mask[5] = 1U;
    return seed_mask;
}
AnnotationObject make_capture_space_mask_preview_object(const std::uint64_t seed_frame_id, const std::optional<ContentIdentity>& seed_live_frame_id) {
    AnnotationObject instance =
        make_mask_object(AnnotationBox{1, 1, 3, 3}, AnnotationMaskRegion{0, 0, 4, 4}, make_single_pixel_seed_mask(), seed_frame_id, seed_live_frame_id);
    instance.category_index = 0U;
    return instance;
}
std::vector<AnnotationResolvedObject> make_capture_space_preview_result(const AnnotationFrame& cropped, const std::uint64_t seed_frame_id,
                                                                        const std::optional<ContentIdentity>& seed_live_frame_id, const bool live_mode) {
    return resolve_annotation_objects(cropped, make_single_category("reticle"), {make_capture_space_mask_preview_object(seed_frame_id, seed_live_frame_id)},
                                      live_mode);
}
const AnnotationResolvedObject& require_single_resolved_object(const std::vector<AnnotationResolvedObject>& resolved_objects) {
    REQUIRE(resolved_objects.size() == 1U);
    return resolved_objects.front();
}
void assert_resolved_bbox_and_mask(const AnnotationResolvedObject& resolved, const AnnotationBox& bbox, const std::string_view mask_rle) {
    REQUIRE(resolved.bbox.x1 == bbox.x1);
    REQUIRE(resolved.bbox.y1 == bbox.y1);
    REQUIRE(resolved.bbox.x2 == bbox.x2);
    REQUIRE(resolved.bbox.y2 == bbox.y2);
    REQUIRE(resolved.mask_rle == mask_rle);
}
void assert_single_resolved_bbox_and_mask(const std::vector<AnnotationResolvedObject>& resolved_objects, const AnnotationBox& bbox,
                                          const std::string_view mask_rle) {
    assert_resolved_bbox_and_mask(require_single_resolved_object(resolved_objects), bbox, mask_rle);
}
std::vector<AnnotationResolvedObject> build_reticle_preview(const AnnotationFrame& frame, AnnotationObject object, const bool live_mode = false) {
    return resolve_annotation_objects(frame, make_single_category("reticle"), {std::move(object)}, live_mode);
}
void assert_frame_window(const AnnotationFrame& frame, const std::uint32_t width, const std::uint32_t height, const std::uint32_t view_x,
                         const std::uint32_t view_y, const std::uint32_t capture_width, const std::uint32_t capture_height) {
    REQUIRE(frame.width == width);
    REQUIRE(frame.height == height);
    REQUIRE(frame.view_x == view_x);
    REQUIRE(frame.view_y == view_y);
    REQUIRE(frame.capture_width == capture_width);
    REQUIRE(frame.capture_height == capture_height);
}
struct ReloadedScene {
    std::vector<AnnotationObject> objects;
    AnnotationCategories categories;
};
// Saves a single-object scene under a fresh temp root and reloads it, requiring exactly one loaded
// object and the expected single category name.
ReloadedScene save_and_reload_single_object(const char* temp_name, const char* split, const AnnotationFrame& frame, AnnotationCategories categories,
                                            const AnnotationObject& object, const char* expected_category_name) {
    const ScopedTempDir temporary{temp_name};
    const fs::path& temp_root = temporary.path();
    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = split;
    const AnnotationSaveResult save = save_annotation_scene(config, frame, categories, {object}, false);
    ReloadedScene scene;
    scene.categories = load_annotation_categories(temp_root);
    scene.objects = load_annotation_scene_objects(save.scene_jsonl_path, &scene.categories);
    REQUIRE(scene.objects.size() == 1U);
    REQUIRE(scene.categories.items.size() == 1U);
    REQUIRE(scene.categories.items.front().name == expected_category_name);
    return scene;
}
TEST_CASE("test_recenter_resets_tolerances", "[backend][imaging][annotation]") {
    AnnotationColorRange range;
    range.tolerance.hue_minus_pct = 12.0f;
    range.tolerance.value_plus_pct = 18.0f;
    range.sampling = true;
    recenter_annotation_range(range, AnnotationHsv{30.0f, 0.75f, 0.25f});
    REQUIRE(range.center.hue_degrees == 30.0f);
    REQUIRE(range.center.saturation == 0.75f);
    REQUIRE(range.center.value == 0.25f);
    REQUIRE(!annotation_range_active(range));
    REQUIRE(!range.sampling);
}
TEST_CASE("test_preview_builds_mask_from_box_minus_sup", "[backend][imaging][annotation]") {
    const std::vector<AnnotationResolvedObject> preview = build_reticle_preview(make_frame(), make_reticle_box_preview_object(AnnotationBox{0, 0, 4, 2}, true));
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{2, 0, 4, 2}, "2:2 6:2");
}
TEST_CASE("test_preview_nosup_restores_suppressed_pixels", "[backend][imaging][annotation]") {
    const std::vector<AnnotationResolvedObject> preview =
        build_reticle_preview(make_frame(), make_reticle_box_preview_object(AnnotationBox{0, 0, 2, 2}, true, true));
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{0, 0, 2, 2}, "0:2 4:2");
}
TEST_CASE("test_resolved_crop_preserves_mask_alpha", "[backend][imaging][annotation]") {
    const std::vector<AnnotationResolvedObject> preview = build_reticle_preview(make_frame(), make_reticle_box_preview_object(AnnotationBox{0, 0, 3, 3}, true));
    const AnnotationResolvedObject& resolved = require_single_resolved_object(preview);
    REQUIRE(resolved.crop_width == 3U);
    REQUIRE(resolved.crop_height == 3U);
    REQUIRE(resolved.crop_rgba.size() == (static_cast<size_t>(3U) * 3U * 4U));
    REQUIRE(resolved.crop_rgba[3] == 0U);
    REQUIRE(resolved.crop_rgba[7] == 0U);
    REQUIRE(resolved.crop_rgba[15] == 0U);
    REQUIRE(resolved.crop_rgba[19] == 0U);
    REQUIRE(resolved.crop_rgba[11] == 255U);
    REQUIRE(resolved.crop_rgba[23] == 255U);
    REQUIRE(resolved.crop_rgba[35] == 255U);
}
TEST_CASE("test_prediction_mask_decode_and_bbox", "[backend][imaging][annotation]") {
    AnnotationEncodedMask encoded;
    encoded.width = 4;
    encoded.height = 4;
    encoded.runs = {{5U, 2U}, {9U, 1U}};
    const std::vector<std::uint8_t> dense = decode_annotation_prediction_mask(encoded, 4, 4);
    const std::optional<AnnotationBox> bbox = annotation_bbox_from_mask(dense, 4, 4);
    if (!bbox.has_value()) { throw std::runtime_error("expected decoded prediction mask bbox"); }
    const AnnotationBox bbox_value = bbox.value();
    REQUIRE(bbox_value.x1 == 1);
    REQUIRE(bbox_value.y1 == 1);
    REQUIRE(bbox_value.x2 == 3);
    REQUIRE(bbox_value.y2 == 3);
    REQUIRE(encode_annotation_mask_rle(dense) == "5:2 9:1");
}
TEST_CASE("test_mask_rle_round_trip_decode", "[backend][imaging][annotation]") {
    const std::vector<std::uint8_t> mask = {
        0, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1,
    };
    const std::string encoded = encode_annotation_mask_rle(mask);
    const std::vector<std::uint8_t> decoded = decode_annotation_mask_rle(encoded, 4, 4);
    REQUIRE(decoded == mask);
}
TEST_CASE("test_capture_space_box_projects_into_cropped_frame", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = make_cropped_frame(AnnotationBox{2, 0, 4, 2});
    assert_frame_window(cropped, 2U, 2U, 2U, 0U, 4U, 4U);
    const std::vector<AnnotationResolvedObject> preview = build_reticle_preview(cropped, make_reticle_box_preview_object(AnnotationBox{2, 0, 4, 2}));
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{0, 0, 2, 2}, "0:4");
}
TEST_CASE("test_capture_space_model_mask_projects_into_cropped_frame", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = make_capture_space_frame();
    const std::vector<AnnotationResolvedObject> preview = make_capture_space_preview_result(cropped, cropped.frame_id, std::nullopt, false);
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{0, 0, 1, 1}, "0:1");
}
[[nodiscard]] std::shared_ptr<AnnotationDeferredMask> make_deferred_mask() {
    auto deferred = std::make_shared<AnnotationDeferredMask>();
    deferred->source_width = 4U;
    deferred->source_height = 4U;
    deferred->source_crop_width = 4U;
    deferred->source_crop_height = 4U;
    deferred->output_width = 4U;
    deferred->output_height = 4U;
    return deferred;
}
TEST_CASE("test_deferred_model_mask_stays_compact_during_initial_preview_resolution", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = make_frame();
    auto deferred = make_deferred_mask();
    deferred->runs = {{5U, 1U}};
    AnnotationObject object = make_mask_object(AnnotationBox{0, 0, 2, 2}, AnnotationMaskRegion{0, 0, 2, 2}, {}, cropped.frame_id, std::nullopt);
    std::get<AnnotationMaskShape>(object.shape).deferred = deferred;
    const std::vector<AnnotationResolvedObject> preview = build_reticle_preview(cropped, object);
    const AnnotationResolvedObject& resolved = require_single_resolved_object(preview);
    REQUIRE((resolved.bbox.x1 == 0 && resolved.bbox.y1 == 0 && resolved.bbox.x2 == 2 && resolved.bbox.y2 == 2));
    REQUIRE(resolved.mask.empty());
    REQUIRE(resolved.mask_rle.empty());
    REQUIRE(resolved.crop_rgba.empty());
    const AnnotationMaskShape& persistent = std::get<AnnotationMaskShape>(object.shape);
    REQUIRE(persistent.mask.empty());
    REQUIRE(persistent.deferred == deferred);
}
TEST_CASE("test_invalid_deferred_model_mask_has_deterministic_box_preview", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = make_capture_space_frame();
    auto deferred = make_deferred_mask();
    deferred->view_x = 1U;
    deferred->view_y = 1U;
    deferred->runs = {{15U, 2U}};
    AnnotationObject object = make_mask_object(AnnotationBox{0, 0, 2, 2}, AnnotationMaskRegion{0, 0, 2, 2}, {}, cropped.frame_id, std::nullopt);
    std::get<AnnotationMaskShape>(object.shape).deferred = deferred;
    const std::vector<AnnotationResolvedObject> preview = build_reticle_preview(cropped, std::move(object));
    const AnnotationResolvedObject& resolved = require_single_resolved_object(preview);
    REQUIRE(resolved.mask.empty());
    REQUIRE(resolved.mask_rle.empty());
    REQUIRE(resolved.crop_rgba.empty());
}
TEST_CASE("test_persistent_capture_space_model_mask_survives_live_mode", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = make_capture_space_frame();
    const std::vector<AnnotationResolvedObject> preview = make_capture_space_preview_result(cropped, 0U, std::nullopt, true);
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{0, 0, 1, 1}, "0:1");
}
TEST_CASE("test_live_model_mask_requires_matching_live_frame_identity", "[backend][imaging][annotation]") {
    AnnotationFrame cropped = make_capture_space_frame();
    cropped.live_frame_id = ContentIdentity{41U, cropped.frame_id};
    const std::vector<AnnotationResolvedObject> preview =
        make_capture_space_preview_result(cropped, cropped.frame_id, ContentIdentity{99U, cropped.frame_id}, true);
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{0, 0, 2, 2}, "0:4");
}
TEST_CASE("test_live_model_mask_uses_matching_live_frame_identity_even_if_display_id_drifts", "[backend][imaging][annotation]") {
    AnnotationFrame cropped = make_capture_space_frame();
    cropped.live_frame_id = ContentIdentity{41U, cropped.frame_id};
    const std::vector<AnnotationResolvedObject> preview = make_capture_space_preview_result(cropped, cropped.frame_id + 100U, cropped.live_frame_id, true);
    assert_single_resolved_bbox_and_mask(preview, AnnotationBox{0, 0, 1, 1}, "0:1");
}
TEST_CASE("test_box_round_trip_between_capture_and_frame_space", "[backend][imaging][annotation]") {
    AnnotationFrame frame = extract_annotation_frame_region(make_frame(), AnnotationBox{1, 1, 4, 4});
    const AnnotationBox capture_box{2, 2, 4, 4};
    const AnnotationBox frame_box = annotation_box_to_frame(frame, capture_box);
    REQUIRE(frame_box.x1 == 1);
    REQUIRE(frame_box.y1 == 1);
    REQUIRE(frame_box.x2 == 3);
    REQUIRE(frame_box.y2 == 3);
    const AnnotationBox round_trip = annotation_box_from_frame(frame, frame_box);
    REQUIRE(round_trip.x1 == capture_box.x1);
    REQUIRE(round_trip.y1 == capture_box.y1);
    REQUIRE(round_trip.x2 == capture_box.x2);
    REQUIRE(round_trip.y2 == capture_box.y2);
}
TEST_CASE("test_save_scene_writes_outputs", "[backend][imaging][annotation]") {
    AnnotationCategories categories;
    ensure_annotation_category(categories, "reticle");
    AnnotationFrame frame = make_frame();
    AnnotationObject instance = make_box_object(AnnotationBox{2, 0, 4, 2});
    instance.category_index = 0;
    const ScopedTempDir temporary{"mmltk-test-gui-annotation-core"};
    const fs::path& temp_root = temporary.path();
    AnnotationSaveConfig config;
    config.output_root = temp_root;
    config.split = "train";
    const AnnotationSaveResult save = save_annotation_scene(config, frame, categories, {instance}, false);
    REQUIRE(fs::exists(save.scene_image_path));
    REQUIRE(fs::exists(save.scene_jsonl_path));
    REQUIRE(save.entity_paths.size() == 1U);
    REQUIRE(fs::exists(save.entity_paths.front()));
    REQUIRE(fs::exists(temp_root / "categories.json"));
    REQUIRE(fs::exists(temp_root / "manifests" / "scenes.jsonl"));
    REQUIRE(fs::exists(temp_root / "manifests" / "entities.jsonl"));
    AnnotationCategories loaded_categories = load_annotation_categories(temp_root);
    const std::optional<std::vector<AnnotationObject>> loaded_objects = load_saved_annotation_scene_for_frame(temp_root, frame, &loaded_categories);
    if (!loaded_objects.has_value()) { throw std::runtime_error("expected saved annotation scene to reload"); }
    const auto& loaded_objects_value = loaded_objects.value();
    REQUIRE(loaded_objects_value.size() == 1U);
    const AnnotationBoxShape* loaded_box = std::get_if<AnnotationBoxShape>(&loaded_objects_value.front().shape);
    REQUIRE(loaded_box != nullptr);
    REQUIRE(loaded_box->box.x1 == 2);
    REQUIRE(loaded_box->box.y1 == 0);
    REQUIRE(loaded_box->box.x2 == 4);
    REQUIRE(loaded_box->box.y2 == 2);
}
TEST_CASE("test_scene_round_trip_preserves_point_capture_space", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = extract_annotation_frame_region(make_large_frame(), AnnotationBox{4, 3, 20, 18});
    AnnotationObject point;
    point.object_id = "manual-1";
    point.category_index = 0U;
    point.shape = AnnotationPointShape{AnnotationPoint{7.0f, 9.0f}};
    const ReloadedScene scene =
        save_and_reload_single_object("mmltk-test-gui-annotation-point", "train", cropped, make_single_category("point"), point, "point");
    const AnnotationPointShape* loaded_point = std::get_if<AnnotationPointShape>(&scene.objects.front().shape);
    REQUIRE(loaded_point != nullptr);
    REQUIRE(loaded_point->point.x == 7.0f);
    REQUIRE(loaded_point->point.y == 9.0f);
}
TEST_CASE("test_load_annotation_categories_rejects_missing_required_schema_fields", "[backend][imaging][annotation]") {
    const ScopedTempDir temporary{"mmltk-test-gui-annotation-categories"};
    const fs::path& temp_root = temporary.path();
    const fs::path categories_path = temp_root / "categories.json";
    write_text_file(categories_path, R"({"classes":[]})");
    REQUIRE(load_annotation_categories(temp_root).items.empty());
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
    expect_runtime_error_contains([&]() { (void)load_annotation_categories(temp_root); }, "missing array `classes`");
}
TEST_CASE("test_load_annotation_categories_rejects_wrong_schema_version", "[backend][imaging][annotation]") {
    const ScopedTempDir temporary{"mmltk-test-gui-annotation-version"};
    const fs::path& temp_root = temporary.path();
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
    expect_runtime_error_contains([&]() { (void)load_annotation_categories(temp_root); }, "unexpected `version`");
}
TEST_CASE("test_load_annotation_scene_objects_rejects_malformed_shape_records", "[backend][imaging][annotation]") {
    const ScopedTempDir temporary{"mmltk-test-gui-scene-errors"};
    const fs::path& temp_root = temporary.path();
    const fs::path scene_path = temp_root / "scene.jsonl";
    write_text_file(scene_path,
                    R"({"class":"point","shape_type":"point","image_size_wh":[32,24],"view_origin_xy":[0,0],"capture_size_wh":[32,24],"shape":{}}
)");
    expect_runtime_error_contains([&]() { (void)load_annotation_scene_objects(scene_path, nullptr); }, "point record is missing `shape.xy`");
    write_text_file(scene_path,
                    R"({"class":"curve","shape_type":"spline","image_size_wh":[32,24],"view_origin_xy":[0,0],"capture_size_wh":[32,24],"shape":{"closed":false}}
)");
    expect_runtime_error_contains([&]() { (void)load_annotation_scene_objects(scene_path, nullptr); }, "spline record is missing array `shape.knots`");
    write_text_file(scene_path,
                    R"({"class":"pose","shape_type":"skeleton","image_size_wh":[32,24],"view_origin_xy":[0,0],"capture_size_wh":[32,24],"shape":{"nodes":[]}}
)");
    expect_runtime_error_contains([&]() { (void)load_annotation_scene_objects(scene_path, nullptr); }, "skeleton record is missing array `shape.edges`");
}
TEST_CASE("test_scene_round_trip_preserves_spline_topology", "[backend][imaging][annotation]") {
    const AnnotationFrame cropped = extract_annotation_frame_region(make_large_frame(), AnnotationBox{4, 3, 20, 18});
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
    const ReloadedScene scene =
        save_and_reload_single_object("mmltk-test-gui-annotation-spline", "train", cropped, make_single_category("curve"), spline, "curve");
    const AnnotationSplineShape* loaded_spline = std::get_if<AnnotationSplineShape>(&scene.objects.front().shape);
    REQUIRE(loaded_spline != nullptr);
    REQUIRE(loaded_spline->closed);
    REQUIRE(loaded_spline->knots.size() == 2U);
    REQUIRE(loaded_spline->knots[0].position.x == 6.0f);
    REQUIRE(loaded_spline->knots[0].position.y == 5.0f);
    REQUIRE(loaded_spline->knots[0].in_handle.enabled);
    REQUIRE(loaded_spline->knots[0].in_handle.position.x == 5.0f);
    REQUIRE(loaded_spline->knots[0].in_handle.position.y == 4.0f);
    REQUIRE(loaded_spline->knots[0].out_handle.enabled);
    REQUIRE(loaded_spline->knots[0].out_handle.position.x == 9.0f);
    REQUIRE(loaded_spline->knots[0].out_handle.position.y == 5.5f);
    REQUIRE(loaded_spline->knots[0].handle_mode == AnnotationSplineHandleMode::Smooth);
    REQUIRE(loaded_spline->knots[1].position.x == 14.0f);
    REQUIRE(loaded_spline->knots[1].position.y == 11.0f);
    REQUIRE(loaded_spline->knots[1].in_handle.enabled);
    REQUIRE(loaded_spline->knots[1].out_handle.enabled);
    REQUIRE(loaded_spline->knots[1].handle_mode == AnnotationSplineHandleMode::Mirrored);
}
TEST_CASE("test_scene_round_trip_preserves_skeleton_topology", "[backend][imaging][annotation]") {
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
    const ReloadedScene scene = save_and_reload_single_object("mmltk-test-gui-annotation-skeleton", "val", make_large_frame(), categories, skeleton, "pose");
    const AnnotationSkeletonShape* loaded_skeleton = std::get_if<AnnotationSkeletonShape>(&scene.objects.front().shape);
    REQUIRE(loaded_skeleton != nullptr);
    REQUIRE(loaded_skeleton->nodes.size() == 3U);
    REQUIRE(loaded_skeleton->edges.size() == 2U);
    REQUIRE(loaded_skeleton->nodes[0].key == "left");
    REQUIRE(loaded_skeleton->nodes[0].visible);
    REQUIRE(loaded_skeleton->nodes[2].key == "tail");
    REQUIRE(!loaded_skeleton->nodes[2].visible);
    REQUIRE(loaded_skeleton->edges[1].source_index == 1U);
    REQUIRE(loaded_skeleton->edges[1].target_index == 2U);
    REQUIRE(scene.categories.items.size() == 1U);
    REQUIRE(scene.categories.items[0].keypoints.size() == 3U);
    REQUIRE(scene.categories.items[0].skeleton_edges.size() == 2U);
}
}  // namespace
TEST_CASE("test_dense_source_catalog_reorder_preserves_jsonl_meaning", "[backend][imaging][annotation]") {
    const ScopedTempDir temporary{"mmltk-test-source-catalog"};
    const auto root = temporary.path();
    write_text_file(root / "categories.json", R"({"classes":[{"id":2,"name":"dog","keypoints":["nose"],"skeleton_edges":[]},{"id":1,"name":"background"}]})");
    auto categories = load_annotation_categories(root);
    REQUIRE(categories.items[0].id == 2);
    REQUIRE(categories.items[1].id == 1);
    REQUIRE(categories.items[0].keypoints == std::vector<std::string>{"nose"});
    REQUIRE(ensure_annotation_category(categories, "cat") == 2U);
    REQUIRE(categories.items[2].id == 3);
    write_annotation_categories(root, categories);
    const auto reopened = load_annotation_categories(root);
    REQUIRE(reopened.items[0].id == 2);
    REQUIRE(reopened.items[1].name == "background");
    REQUIRE(reopened.items[2].name == "cat");
    write_text_file(root / "empty.jsonl", "");
    REQUIRE(load_annotation_scene_objects(root / "empty.jsonl", &categories).empty());
    REQUIRE(categories.items.size() == 3U);
    for (const auto text : {R"({"classes":[{"id":0,"name":"same"},{"id":1,"name":"same"}]})", R"({"classes":[{"id":1,"name":"a"},{"id":1,"name":"b"}]})",
                            R"({"classes":[{"id":1,"name":"a"},{"id":3,"name":"b"}]})", R"({"classes":[{"id":0,"name":""}]})"}) {
        write_text_file(root / "categories.json", text);
        REQUIRE_THROWS(load_annotation_categories(root));
    }
}
