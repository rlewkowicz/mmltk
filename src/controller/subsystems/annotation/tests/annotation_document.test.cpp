#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <utility>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "src/acceptance/tests/filesystem_test_utils.hpp"
#include "src/controller/presentation/visual_document.h"
#include "src/controller/subsystems/annotation/detail/annotation_document.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/frameworks/serialization/serialization.h"

namespace mmltk::controller {
namespace {

[[nodiscard]] contracts::AnnotationSceneContent test_scene(const std::string_view identity) {
    return {
        .document = contracts::WorkspaceResource::From(identity, 1U),
        .categories = {{.value = "object"}},
        .frame_width = 64U,
        .frame_height = 64U,
        .frame_ready = true,
    };
}

void apply_gesture(subsystems::annotation::AnnotationDocument& editor, AnnotationPointer& pointer, contracts::AnnotationPoint end,
                   contracts::AnnotationPointerPhase phase = contracts::AnnotationPointerPhase::End) {
    REQUIRE(editor.Pointer(pointer).outcome == subsystems::annotation::DocumentOutcome::Applied);
    pointer.phase = phase;
    ++pointer.sequence;
    pointer.point = end;
    REQUIRE(editor.Pointer(pointer).outcome == subsystems::annotation::DocumentOutcome::Applied);
}

TEST_CASE("Annotation private document owns pointer history and peer cancellation") {
    namespace document = subsystems::annotation;
    document::AnnotationDocument editor;
    auto scene = test_scene("direct://annotation-test");
    REQUIRE(editor.Open(std::move(scene)).outcome == document::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = AnnotationToolEdit{contracts::AnnotationTool::Box}}).outcome == document::DocumentOutcome::Applied);
    AnnotationPointer pointer{
        .phase = contracts::AnnotationPointerPhase::Begin,
        .interaction_id = 1U,
        .sequence = 1U,
        .point = {4.0F, 5.0F},
    };
    REQUIRE(editor.Pointer(pointer).outcome == document::DocumentOutcome::Applied);
    editor.PeerClosed();
    pointer.interaction_id = 2U;
    const auto pointer_revision = editor.ui().interaction_revision;
    const auto pointer_ui = editor.ui();
    REQUIRE(editor.Pointer(pointer).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().interaction_revision == pointer_revision);
    pointer.phase = contracts::AnnotationPointerPhase::Update;
    pointer.sequence = 2U;
    pointer.point = {12.0F, 14.0F};
    REQUIRE(editor.Pointer(pointer).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().interaction_revision == pointer_revision);
    CHECK(editor.ui() == pointer_ui);
    pointer.phase = contracts::AnnotationPointerPhase::End;
    pointer.sequence = 3U;
    pointer.point = {20.0F, 24.0F};
    REQUIRE(editor.Pointer(pointer).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().interaction_revision == pointer_revision + 1U);
    REQUIRE(editor.ui().scene.objects.size() == 1U);
    REQUIRE(editor.Edit({.value = AnnotationUndoEdit{}}).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.empty());
    REQUIRE(editor.Edit({.value = AnnotationRedoEdit{}}).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.size() == 1U);
    auto reopened = editor.ui().scene;
    REQUIRE(editor.Open(std::move(reopened)).outcome == document::DocumentOutcome::Applied);
    CHECK_FALSE(editor.ui().tool_capabilities.empty());
    CHECK(editor.ui().valid());
}

TEST_CASE("Annotation private document enforces the canonical fixed text policy") {
    namespace document = subsystems::annotation;
    document::AnnotationDocument editor;
    auto scene = test_scene("direct://annotation-text");
    REQUIRE(editor.Open(std::move(scene)).outcome == document::DocumentOutcome::Applied);
    const auto apply = [&editor](contracts::AnnotationText value) {
        return editor.Edit({.value = AnnotationCategoryEdit{std::move(value)}}).outcome;
    };

    CHECK(apply({}) == document::DocumentOutcome::Rejected);
    auto over_capacity = contracts::AnnotationText::From("x");
    over_capacity.size = static_cast<std::uint8_t>(over_capacity.bytes.size() + 1U);
    CHECK(apply(over_capacity) == document::DocumentOutcome::Rejected);
    auto control = contracts::AnnotationText{};
    control.bytes[0] = '\n';
    control.size = 1U;
    CHECK(apply(control) == document::DocumentOutcome::Rejected);
    auto nonzero_tail = contracts::AnnotationText::From("x");
    nonzero_tail.bytes[1] = 'y';
    CHECK(apply(nonzero_tail) == document::DocumentOutcome::Rejected);
    CHECK(apply(contracts::AnnotationText::From("category")) == document::DocumentOutcome::Applied);
}

TEST_CASE("Visual document projection transforms every canonical spatial member with clipped crop coordinates") {
    auto source = std::make_shared<VisualDocument>();
    source->scene = test_scene("direct://all-geometry");
    contracts::AnnotationObject object{
        .name = contracts::AnnotationText::From("geometry"),
        .shape = contracts::AnnotationShape::Spline,
        .box = {{4.0F, 8.0F}, {60.0F, 64.0F}},
        .point = {20.0F, 24.0F},
        .mask_points = {{12.0F, 16.0F}, {56.0F, 60.0F}},
        .spline_knots = {{.point = {24.0F, 28.0F}, .in = {{4.0F, 8.0F}, true}, .out = {{60.0F, 64.0F}, true}}},
        .skeleton_nodes = {{.key = contracts::AnnotationText::From("node"), .point = {32.0F, 36.0F}}},
    };
    source->scene.objects.push_back(object);
    REQUIRE(source->scene.valid());
    const auto scaled = scale_visual_document(source, 4U);
    const auto& scaled_object = scaled->scene.objects.front();
    CHECK(scaled_object.mask_points.front() == contracts::AnnotationPoint{48.0F, 64.0F});
    CHECK(scaled_object.spline_knots.front().in.point == contracts::AnnotationPoint{16.0F, 32.0F});
    CHECK(scaled_object.spline_knots.front().out.point == contracts::AnnotationPoint{240.0F, 256.0F});
    CHECK(scaled_object.skeleton_nodes.front().point == contracts::AnnotationPoint{128.0F, 144.0F});
    const auto cropped = materialize_visual_document(*scaled, {256U, 256U}, {32U, 48U, 160U, 144U});
    REQUIRE(cropped.valid());
    CHECK(cropped.frame_width == 160U);
    CHECK(cropped.frame_height == 144U);
    const auto& projected = cropped.objects.front();
    CHECK(projected.box == contracts::AnnotationBox{{0.0F, 0.0F}, {160.0F, 144.0F}});
    CHECK(projected.point == contracts::AnnotationPoint{48.0F, 48.0F});
    CHECK(projected.mask_points == std::vector<contracts::AnnotationPoint>{{16.0F, 16.0F}, {160.0F, 144.0F}});
    CHECK(projected.spline_knots.front().point == contracts::AnnotationPoint{64.0F, 64.0F});
    CHECK(projected.spline_knots.front().in.point == contracts::AnnotationPoint{0.0F, 0.0F});
    CHECK(projected.spline_knots.front().out.point == contracts::AnnotationPoint{160.0F, 144.0F});
    CHECK(projected.skeleton_nodes.front().point == contracts::AnnotationPoint{96.0F, 96.0F});
    CHECK(source->scene.objects.front() == object);
}

TEST_CASE("Viewed masks import full catalogs and retain editable runs through history and atomic save") {
    namespace document = subsystems::annotation;
    auto source = std::make_shared<VisualDocument>();
    source->scene.document = contracts::WorkspaceResource::From("explore://mask", 1U);
    for (std::size_t index = 0U; index < 80U; ++index)
        source->scene.categories.push_back({.value = "class " + std::to_string(index)});
    source->scene.categories.back().value = "étiquette";
    source->scene.palette = contracts::annotation_class_palette(80U);
    source->scene.objects.push_back({
        .name = contracts::AnnotationText::From("striped mask"),
        .shape = contracts::AnnotationShape::Mask,
        .box = {{0.0F, 0.0F}, {64.0F, 64.0F}},
        .mask = {.present = true},
        .category = 79U,
    });
    source->mask_contains = [](std::size_t, float x, float y) {
        const auto row = static_cast<unsigned>(y * 64.0F);
        return (row % 2U) == 0U && x >= 0.125F && x < 0.875F;
    };
    const auto imported = materialize_visual_document(*source, {64U, 64U}, {});
    REQUIRE(imported.valid());
    REQUIRE(imported.objects.front().mask.runs.size() == 32U);
    REQUIRE(imported.categories.size() == 80U);
    CHECK(imported.objects.front().category == 79U);
    CHECK(imported.categories.back().value == "étiquette");
    CHECK(imported.palette == source->scene.palette);

    const auto scaled = scale_visual_document(source, 4U);
    const auto cropped = materialize_visual_document(*scaled, {256U, 256U}, {32U, 32U, 192U, 192U});
    REQUIRE(cropped.valid());
    REQUIRE(cropped.objects.front().mask.runs.size() == 96U);
    CHECK(cropped.objects.front().mask.runs.front() == contracts::AnnotationMaskRun{0U, 0U, 191U});
    CHECK(cropped.palette == imported.palette);

    document::AnnotationDocument editor;
    REQUIRE(editor.Open(imported).outcome == document::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = AnnotationObjectEdit{0}}).outcome == document::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = AnnotationToolEdit{contracts::AnnotationTool::MaskErase}}).outcome == document::DocumentOutcome::Applied);
    AnnotationPointer pointer{
        .phase = contracts::AnnotationPointerPhase::Begin,
        .interaction_id = 1U,
        .sequence = 1U,
        .target = {.object = 0U},
        .point = {32.0F, 4.0F},
    };
    REQUIRE(editor.Pointer(pointer).outcome == document::DocumentOutcome::Applied);
    pointer.phase = contracts::AnnotationPointerPhase::End;
    pointer.sequence = 2U;
    REQUIRE(editor.Pointer(pointer).outcome == document::DocumentOutcome::Applied);
    REQUIRE(editor.ui().scene.objects.front().mask != imported.objects.front().mask);
    CHECK(std::ranges::none_of(editor.ui().scene.objects.front().mask.runs,
                               [](auto run) { return run.row == 4 && run.first <= 32 && run.last >= 32; }));
    const auto edited = editor.ui().scene;
    REQUIRE(editor.Edit({.value = AnnotationUndoEdit{}}).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.front().mask == imported.objects.front().mask);
    REQUIRE(editor.Edit({.value = AnnotationRedoEdit{}}).outcome == document::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects == edited.objects);
    CHECK(editor.ui().scene.categories == edited.categories);
    CHECK(editor.ui().scene.palette == edited.palette);
    CHECK(editor.ui().scene.document.revision > edited.document.revision);
    const auto saved_scene = editor.ui().scene;

    mmltk::testsupport::ScopedTempDir directory{"annotation-mask"};
    const auto path = directory.path() / "document.cbor";
    REQUIRE(editor.Save(path.string()).outcome == document::DocumentOutcome::Applied);
    std::ifstream file{path, std::ios::binary};
    const std::vector<char> characters{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const auto bytes = std::as_bytes(std::span{characters});
    const auto decoded = mmltk::frameworks::serialization::decode<contracts::AnnotationUiState>(
        {.first = bytes}, {.max_bytes = contracts::kAnnotationUiStateByteBudget, .max_items = contracts::kAnnotationUiStateByteBudget});
    REQUIRE(decoded.has_value());
    CHECK(decoded->scene == saved_scene);
    CHECK(editor.Save((directory.path() / "absent" / "document.cbor").string()).outcome == document::DocumentOutcome::Rejected);
    CHECK(editor.ui().scene == saved_scene);
}

TEST_CASE("Annotation import capacity failure leaves the open editable document unchanged") {
    namespace document = subsystems::annotation;
    document::AnnotationDocument editor;
    REQUIRE(editor.Open(test_scene("direct://kept")).outcome == document::DocumentOutcome::Applied);
    const auto kept = editor.ui();
    auto excessive = test_scene("direct://too-many");
    excessive.categories.resize(contracts::kAnnotationCategoryCapacity + 1U, contracts::ArtifactClassName{.value = "category"});
    CHECK(editor.Open(excessive).outcome != document::DocumentOutcome::Applied);
    CHECK(editor.ui() == kept);
    excessive = test_scene("direct://too-many-runs");
    excessive.objects.push_back(
        {.name = contracts::AnnotationText::From("mask"),
         .shape = contracts::AnnotationShape::Mask,
         .mask = {.runs = std::vector<contracts::AnnotationMaskRun>(contracts::kAnnotationMaskRunCapacity + 1U, {1U, 1U, 2U}),
                  .present = true}});
    CHECK(editor.Open(std::move(excessive)).outcome != document::DocumentOutcome::Applied);
    CHECK(editor.ui() == kept);
    // Inject malformed current facts through the existing read-only observation
    // solely to verify mutation and persistence boundaries reject them unchanged.
    auto& malformed = const_cast<contracts::AnnotationUiState&>(editor.ui());
    malformed.editor.selected_object = 0U;
    const auto invalid = malformed;
    CHECK(editor.Edit({.value = AnnotationCategoryEdit{contracts::AnnotationText::From("refused")}}).outcome == document::DocumentOutcome::Rejected);
    mmltk::testsupport::ScopedTempDir directory{"annotation-invalid-current"};
    const auto destination = directory.path() / "invalid.cbor";
    CHECK(editor.Save(destination.string()).outcome == document::DocumentOutcome::Rejected);
    CHECK_FALSE(std::filesystem::exists(destination));
    CHECK(editor.ui() == invalid);
    malformed = kept;
    CHECK(editor.ui() == kept);

}

TEST_CASE("The native maximum object scene fits the bounded editable snapshot and persistence policy") {
    auto scene = test_scene("direct://maximum-scene");
    scene.objects.resize(contracts::kAnnotationObjectCapacity,
                         {.name = contracts::AnnotationText::From("box"), .box = {{1.0F, 1.0F}, {63.0F, 63.0F}}});
    subsystems::annotation::AnnotationDocument editor;
    REQUIRE(editor.Open(std::move(scene)).outcome == subsystems::annotation::DocumentOutcome::Applied);
    std::vector<std::byte> encoded;
    REQUIRE(contracts::encode_annotation_persistence(editor.ui(), encoded));
    CHECK(encoded.size() <= contracts::kAnnotationUiStateByteBudget);
}

}  // namespace
}  // namespace mmltk::controller

TEST_CASE("Annotation valid repeated edits do not consume history and editing outlives the journal") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    d::AnnotationDocument editor;
    c::contracts::AnnotationSceneContent scene{.document = c::contracts::WorkspaceResource::From("test://history", 1U),
                                               .categories = {{.value = "vehicle"}},
                                               .objects = {{.name = c::contracts::AnnotationText::From("box")}},
                                               .frame_width = 64,
                                               .frame_height = 64,
                                               .frame_ready = true};
    REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = c::AnnotationObjectEdit{0}}).outcome == d::DocumentOutcome::Applied);
    const auto revision = editor.ui().document_revision;
    CHECK(editor.Edit({.value = c::AnnotationObjectEdit{0}}).outcome == d::DocumentOutcome::Applied);
    CHECK(editor.ui().document_revision == revision);
    for (unsigned index = 0; index < 80; ++index)
        REQUIRE(editor.Edit({.value = c::AnnotationSelectedObjectEdit{0, index % 2 != 0}}).outcome == d::DocumentOutcome::Applied);
    for (unsigned index = 0; index < 32; ++index)
        REQUIRE(editor.Edit({.value = c::AnnotationUndoEdit{}}).outcome == d::DocumentOutcome::Applied);
    CHECK(editor.Edit({.value = c::AnnotationUndoEdit{}}).outcome == d::DocumentOutcome::Applied);
}

TEST_CASE("Annotation brush follows every segment as one cancelable transaction") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    d::AnnotationDocument editor;
    c::contracts::AnnotationSceneContent scene{.document = c::contracts::WorkspaceResource::From("test://brush", 1U),
                                               .categories = {{.value = "vehicle"}},
                                               .frame_width = 64,
                                               .frame_height = 64,
                                               .frame_ready = true};
    REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = c::AnnotationToolEdit{c::contracts::AnnotationTool::MaskPaint}}).outcome == d::DocumentOutcome::Applied);
    c::AnnotationPointer pointer{.interaction_id = 1, .sequence = 1, .point = {10, 10}, .brush_radius = 2};
    REQUIRE(editor.Pointer(pointer).outcome == d::DocumentOutcome::Applied);
    pointer.phase = c::contracts::AnnotationPointerPhase::Update;
    pointer.sequence = 2;
    pointer.point = {30, 10};
    REQUIRE(editor.Pointer(pointer).outcome == d::DocumentOutcome::Applied);
    pointer.phase = c::contracts::AnnotationPointerPhase::End;
    pointer.sequence = 3;
    pointer.point = {30, 30};
    REQUIRE(editor.Pointer(pointer).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.ui().scene.objects.size() == 1);
    const auto painted = editor.ui().scene.objects.front();
    CHECK(painted.shape == c::contracts::AnnotationShape::Mask);
    CHECK(std::ranges::any_of(painted.mask.runs, [](auto run) { return run.row == 10 && run.first <= 20 && run.last >= 20; }));
    REQUIRE(editor.Edit({.value = c::AnnotationUndoEdit{}}).outcome == d::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.empty());
    REQUIRE(editor.Edit({.value = c::AnnotationRedoEdit{}}).outcome == d::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.front() == painted);
    pointer = {.interaction_id = 2, .sequence = 1, .target = {.object = 0}, .point = {50, 50}, .brush_radius = 2};
    c::apply_gesture(editor, pointer, pointer.point, c::contracts::AnnotationPointerPhase::Cancel);
    CHECK(editor.ui().scene.objects.front() == painted);
}

TEST_CASE("Annotation creates each supported shape in the selected native class") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    for (auto tool : {c::contracts::AnnotationTool::Box, c::contracts::AnnotationTool::Point, c::contracts::AnnotationTool::Spline,
                      c::contracts::AnnotationTool::Skeleton, c::contracts::AnnotationTool::MaskPaint}) {
        d::AnnotationDocument editor;
        c::contracts::AnnotationSceneContent scene{.document = c::contracts::WorkspaceResource::From("test://create", 1U),
                                                   .categories = {{.value = "first"}, {.value = "second"}},
                                                   .palette = {{0, 1, 1}, {200, 0.5F, 0.8F}},
                                                   .frame_width = 64,
                                                   .frame_height = 64,
                                                   .frame_ready = true};
        REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
        CHECK(editor.ui().scene.palette == scene.palette);
        CHECK(editor.ui().editor.selected_category == 0);
        REQUIRE(editor.Edit({.value = c::AnnotationClassEdit{1}}).outcome == d::DocumentOutcome::Applied);
        REQUIRE(editor.Edit({.value = c::AnnotationToolEdit{tool}}).outcome == d::DocumentOutcome::Applied);
        c::AnnotationPointer pointer{.interaction_id = 1, .sequence = 1, .point = {10, 10}};
        c::apply_gesture(editor, pointer, {20, 20});
        REQUIRE(editor.ui().scene.objects.size() == 1);
        CHECK(editor.ui().scene.objects.front().category == 1);
        CHECK(editor.ui().scene.valid());
        REQUIRE(editor.Edit({.value = c::AnnotationUndoEdit{}}).outcome == d::DocumentOutcome::Applied);
        CHECK(editor.ui().scene.objects.empty());
    }
}

TEST_CASE("Annotation mask fill and cleanup modify support and preserve tight pixel-edge bounds") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    d::AnnotationDocument editor;
    c::contracts::AnnotationObject object{.name = c::contracts::AnnotationText::From("ring"),
                                          .shape = c::contracts::AnnotationShape::Mask,
                                          .box = {{10, 10}, {15, 15}},
                                          .mask = {.runs = {{10, 10, 14},
                                                            {11, 10, 10},
                                                            {11, 14, 14},
                                                            {12, 10, 10},
                                                            {12, 14, 14},
                                                            {13, 10, 10},
                                                            {13, 14, 14},
                                                            {14, 10, 14},
                                                            {40, 40, 40}},
                                                   .present = true}};
    c::contracts::AnnotationSceneContent scene{.document = c::contracts::WorkspaceResource::From("test://cleanup", 1U),
                                               .categories = {{.value = "mask"}},
                                               .objects = {object},
                                               .frame_width = 64,
                                               .frame_height = 64,
                                               .frame_ready = true};
    REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = c::AnnotationObjectEdit{0}}).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = c::AnnotationMaskCleanupEdit{c::contracts::AnnotationMaskCleanup::LargestComponent, 1}}).outcome ==
            d::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.front().box == c::contracts::AnnotationBox{{10, 10}, {15, 15}});
    REQUIRE(editor.Edit({.value = c::AnnotationToolEdit{c::contracts::AnnotationTool::MaskFill}}).outcome == d::DocumentOutcome::Applied);
    c::AnnotationPointer pointer{.interaction_id = 1, .sequence = 1, .target = {.object = 0}, .point = {12, 12}};
    c::apply_gesture(editor, pointer, pointer.point);
    CHECK(editor.ui().scene.objects.front().mask.runs.size() == 5);
    const auto filled_runs = editor.ui().scene.objects.front().mask.runs;
    REQUIRE(editor.Edit({.value = c::AnnotationUndoEdit{}}).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = c::AnnotationMaskCleanupEdit{c::contracts::AnnotationMaskCleanup::FillHoles, 1}}).outcome ==
            d::DocumentOutcome::Applied);
    CHECK(editor.ui().scene.objects.front().mask.runs == filled_runs);
    for (auto operation : {c::contracts::AnnotationMaskCleanup::Dilate, c::contracts::AnnotationMaskCleanup::Erode,
                           c::contracts::AnnotationMaskCleanup::Open, c::contracts::AnnotationMaskCleanup::Close,
                           c::contracts::AnnotationMaskCleanup::FillHoles}) {
        CAPTURE(operation);
        REQUIRE(editor.Edit({.value = c::AnnotationMaskCleanupEdit{operation, 1}}).outcome == d::DocumentOutcome::Applied);
        CHECK(editor.ui().scene.valid());
        const bool expanded = operation == c::contracts::AnnotationMaskCleanup::Dilate;
        const auto first = static_cast<std::uint16_t>(expanded ? 9 : 10);
        const auto last = static_cast<std::uint16_t>(expanded ? 15 : 14);
        const auto& mask = editor.ui().scene.objects.front().mask;
        REQUIRE(mask.runs.size() == static_cast<std::size_t>(last - first + 1U));
        for (std::size_t row = 0U; row < mask.runs.size(); ++row) {
            CAPTURE(row);
            // The radius-one disk adds axial pixels; opening retains rounded corners.
            const bool inset = operation != c::contracts::AnnotationMaskCleanup::Erode && (row == 0U || row + 1U == mask.runs.size());
            CHECK(mask.runs[row].row == first + row);
            CHECK(mask.runs[row].first == first + static_cast<unsigned>(inset));
            CHECK(mask.runs[row].last == last - static_cast<unsigned>(inset));
        }
    }
}

TEST_CASE("Annotation boxes move and resize without changing their pixel content") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    d::AnnotationDocument editor;
    c::contracts::AnnotationSceneContent scene{
        .document = c::contracts::WorkspaceResource::From("test://box", 1),
        .categories = {{.value = "box"}},
        .objects = {{.name = c::contracts::AnnotationText::From("box"), .box = {{10, 10}, {20, 20}}}},
        .frame_width = 64,
        .frame_height = 64,
        .frame_ready = true};
    REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
    c::AnnotationPointer pointer{.interaction_id = 1, .sequence = 1, .target = {.object = 0}, .point = {15, 15}};
    c::apply_gesture(editor, pointer, {25, 25});
    CHECK(editor.ui().scene.objects.front().box == c::contracts::AnnotationBox{{20, 20}, {30, 30}});
    pointer = {.interaction_id = 2,
               .sequence = 1,
               .target = {.object = 0, .element = 2, .role = c::contracts::AnnotationHandleRole::BoxCorner},
               .point = {30, 30}};
    c::apply_gesture(editor, pointer, {40, 45});
    CHECK(editor.ui().scene.objects.front().box == c::contracts::AnnotationBox{{20, 20}, {40, 45}});
}

TEST_CASE("Annotation saves directory targets as stable atomic document files") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    mmltk::testsupport::ScopedTempDir directory{"annotation-output-directory"};
    d::AnnotationDocument editor;
    c::contracts::AnnotationSceneContent scene{.document = c::contracts::WorkspaceResource::From("test://directory", 1)};
    REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
    const auto output = directory.path() / "output";
    REQUIRE(editor.Save(output.string()).outcome == d::DocumentOutcome::Applied);
    REQUIRE(std::filesystem::is_directory(output));
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(output))
        files.push_back(entry.path());
    REQUIRE(files.size() == 1);
    CHECK(files.front().extension() == ".cbor");
    REQUIRE(editor.Save(output.string()).outcome == d::DocumentOutcome::Applied);
    CHECK(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}) == 1);
}

TEST_CASE("Annotation native capabilities reject incompatible typed tools and targets") {
    namespace c = mmltk::controller;
    namespace d = c::subsystems::annotation;
    d::AnnotationDocument editor;
    c::contracts::AnnotationSceneContent scene{.document = c::contracts::WorkspaceResource::From("test://capabilities", 1),
                                               .categories = {{.value = "class"}},
                                               .objects = {{.name = c::contracts::AnnotationText::From("box"), .box = {{1, 1}, {20, 20}}},
                                                           {.name = c::contracts::AnnotationText::From("mask"),
                                                            .shape = c::contracts::AnnotationShape::Mask,
                                                            .mask = {.runs = {{10, 10, 20}}, .present = true}}},
                                               .frame_width = 64,
                                               .frame_height = 64,
                                               .frame_ready = true};
    for (const auto shape :
         {c::contracts::AnnotationShape::Point, c::contracts::AnnotationShape::Spline, c::contracts::AnnotationShape::Skeleton})
        scene.objects.push_back({.name = c::contracts::AnnotationText::From("geometry"), .shape = shape});
    REQUIRE(editor.Open(scene).outcome == d::DocumentOutcome::Applied);
    for (const auto index : std::array<std::uint16_t, 4>{0U, 2U, 3U, 4U}) {
        REQUIRE(editor.Edit({.value = c::AnnotationObjectEdit{index}}).outcome == d::DocumentOutcome::Applied);
        const auto before = editor.ui();
        for (auto tool :
             {c::contracts::AnnotationTool::MaskErase, c::contracts::AnnotationTool::MaskFill, c::contracts::AnnotationTool::ColorSample}) {
            CHECK(editor.Edit({.value = c::AnnotationToolEdit{tool}}).outcome == d::DocumentOutcome::Rejected);
            CHECK(editor.ui() == before);
            CHECK_FALSE(editor.ToolAvailable(tool, index));
        }
        auto colors = before.scene.objects[index].sup;
        colors.center.hue = 120.0F;
        colors.sampling = true;
        CHECK(editor.Edit({.value = c::AnnotationMaskColorsEdit{colors, before.scene.objects[index].nosup}}).outcome ==
              d::DocumentOutcome::Rejected);
        CHECK(editor.ui() == before);
    }
    REQUIRE(editor.Edit({.value = c::AnnotationObjectEdit{1}}).outcome == d::DocumentOutcome::Applied);
    REQUIRE(editor.Edit({.value = c::AnnotationToolEdit{c::contracts::AnnotationTool::ColorSample}}).outcome ==
            d::DocumentOutcome::Applied);
    const auto sampled_before = editor.ui();
    c::AnnotationPointer pointer{.interaction_id = 1, .sequence = 1, .target = {.object = 0}, .point = {10, 10}};
    CHECK(editor.Pointer(pointer).outcome == d::DocumentOutcome::Rejected);
    CHECK(editor.ui() == sampled_before);
    CHECK(editor.ToolAvailable(c::contracts::AnnotationTool::ColorSample, 1));
    CHECK(std::ranges::any_of(editor.ui().tool_capabilities, [](auto capability) {
        return capability.tool == c::contracts::AnnotationTool::ColorSample && capability.available;
    }));
    REQUIRE(editor.Edit({.value = c::AnnotationObjectEdit{0}}).outcome == d::DocumentOutcome::Applied);
    CHECK(editor.ui().editor.tool == c::contracts::AnnotationTool::Select);
}
