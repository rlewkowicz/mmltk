#include "src/backend/models/rfdetr/core/evaluator.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <vector>
#include <nlohmann/json.hpp>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/frameworks/serialization/reflected_cbor.h"
namespace {
namespace data = mmltk::backend::data;
namespace r = mmltk::backend::models::rfdetr;
using Catch::Approx;
r::Prediction box(int category, std::array<float, 4> bounds, float score = 1.0F) {
    r::Prediction result{.class_reference = category, .score = score, .bbox_xyxy = bounds};
    result.has_mask = true;
    r::encode_mask_values_into(128U, 128U, result.mask, [&](std::uint32_t index) {
        const auto x = static_cast<float>(index % 128U), y = static_cast<float>(index / 128U);
        return x >= bounds[0] && x < bounds[2] && y >= bounds[1] && y < bounds[3];
    });
    return result;
}
class EvaluationFixture final {
   public:
    explicit EvaluationFixture(const std::vector<std::vector<r::Prediction>>& images, int resolution = 128,
                               const std::vector<std::vector<nlohmann::json>>& metadata = {}) : root_("evaluator-answers") {
        const data::testsupport::FixtureSpec fixture{root_.path().string(), "train", 128, 128, static_cast<int>(images.size()), 0, 0};
        data::testsupport::create_synthetic_dataset(fixture);
        constexpr std::array names{"person", "ret", "scope", "iron_sight", "anchor_dot", "glint"};
        for (std::size_t image = 0; image < images.size(); ++image) {
            auto name = std::to_string(image + 1U);
            name.insert(0U, 6U - name.size(), '0');
            std::ofstream output(std::filesystem::path(data::testsupport::dataset_dir(fixture)) / "train" / (name + ".jsonl"));
            std::size_t ordinal = 0;
            for (const auto& annotation : images[image]) {
                std::string runs;
                for (const auto& [start, count] : annotation.mask.runs) {
                    if (!runs.empty()) runs += ' ';
                    runs += std::to_string(start) + ':' + std::to_string(count);
                }
                nlohmann::json record{{"class", names[annotation.class_reference]},
                                      {"bbox_xyxy", annotation.bbox_xyxy},
                                      {"image_size_wh", {128, 128}}};
                if (annotation.has_mask) {
                    record["mask_rle_encoding"] = "row_major_start_length";
                    record["mask_rle"] = runs;
                }
                if (!metadata.empty()) record.update(metadata.at(image).at(ordinal));
                ++ordinal;
                output << record.dump() << '\n';
            }
        }
        data::CompilerConfig config;
        config.source_dir = data::testsupport::dataset_dir(fixture);
        config.output_dir = data::testsupport::compiled_dir(fixture);
        config.split = "train";
        config.target_width = resolution;
        config.target_height = resolution;
        config.num_workers = 1;
        const auto plan = data::DatasetCompiler::prepare(config, {config.split});
        data::DatasetCompiler::compile(plan, 0U);
        loader = std::make_unique<data::DatasetLoader>(data::DatasetLoader::Config{.compiled_path = data::testsupport::compiled_bin_path(fixture),
                                                                                   .batch_size = 1U,
                                                                                   .shuffle = false,
                                                                                   .prefetch_factor = 2,
                                                                                   .gather_workers = 1,
                                                                                   .loading = data::data_loading_options(true)});
    }
    std::unique_ptr<data::DatasetLoader> loader;

   private:
    mmltk::testsupport::ScopedTempDir root_;
};
r::EvaluationDatasetOwner::ImageMatches match(r::EvaluationDatasetOwner& owner, std::int64_t image, const std::vector<r::Prediction>& predictions,
                                              std::size_t cap, bool masks = false) {
    std::vector<float> scores, boxes;
    std::vector<std::int64_t> labels;
    for (const auto& prediction : predictions) {
        scores.push_back(prediction.score);
        labels.push_back(prediction.class_reference);
        boxes.insert(boxes.end(), prediction.bbox_xyxy.begin(), prediction.bbox_xyxy.end());
    }
    return owner.match_predictions(image, {static_cast<int>(image + 1), scores.data(), labels.data(), boxes.data(), scores.size()}, std::nullopt, cap,
                                   masks ? std::span<const r::Prediction>{predictions} : std::span<const r::Prediction>{});
}
const r::EvaluationMetricDetail& row(const std::vector<r::EvaluationMetricDetail>& rows, r::EvaluationArea area, std::optional<std::uint32_t> category,
                                     r::EvaluationMetricKind kind = r::EvaluationMetricKind::Box) {
    const auto found = std::ranges::find_if(rows, [&](const auto& value) { return value.area == area && value.category == category && value.kind == kind; });
    REQUIRE(found != rows.end());
    return *found;
}
}  // namespace
TEST_CASE("evaluation distinguishes unavailable categories, measured zero, masks and empty populations", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32})}, {}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBoxAndMask);
    owner.merge_matches(match(owner, 0, {}, 10, true));
    auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.available);
    CHECK(summary.bbox.ap == 0.0);
    REQUIRE(summary.mask);
    CHECK(summary.mask->available);
    CHECK(summary.mask->ap == 0.0);
    auto details = owner.take_details();
    CHECK_FALSE(row(details, r::EvaluationArea::All, 5U).available);
    CHECK(row(details, r::EvaluationArea::Small, 0U).available);
    CHECK(row(details, r::EvaluationArea::Medium, 0U).available);  // Inclusive 32² boundary.
    CHECK_FALSE(row(details, r::EvaluationArea::Large, 0U).available);
    owner.clear_predictions();
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 32, 32})}, 10, true));
    summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.ap == Approx(1.0));
    CHECK(summary.mask->ap == Approx(1.0));
    owner.limit_images(0);
    summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK_FALSE(summary.bbox.available);
    CHECK(summary.bbox.confidence.f1 == 0.0);
    CHECK_FALSE(summary.bbox.average_recall[0]);
}
TEST_CASE("evaluation uses the 101-point envelope and all ten inclusive IoU thresholds", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32}), box(0, {64, 64, 96, 96})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 32, 32})}, 100));
    CHECK(owner.evaluate(100, r::EvaluationDetailRetention::Detailed).bbox.ap == Approx(51.0 / 101.0));
    owner.clear_predictions();
    owner.merge_matches(match(owner, 0, {box(0, {96, 96, 128, 128}, 1.0F), box(0, {0, 0, 32, 32}, 0.9F)}, 100));
    CHECK(owner.evaluate(100, r::EvaluationDetailRetention::Detailed).bbox.ap == Approx(51.0 / 202.0));
    owner.clear_predictions();
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 16, 32})}, 100));
    CHECK(owner.evaluate(100, r::EvaluationDetailRetention::Detailed).bbox.ap == Approx(51.0 / 1010.0));
    auto details = owner.take_details();
    const auto& curve = row(details, r::EvaluationArea::All, 0U);
    CHECK(curve.average_precision[0] == Approx(51.0 / 101.0));
    CHECK(curve.average_precision[1] == 0.0);
    CHECK(curve.precision_curve[0][50] == 1.0);
    CHECK(curve.precision_curve[0][51] == 0.0);
}
TEST_CASE("evaluation shares candidates but keeps independent area ignores and score-prefix caps", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32}), box(0, {0, 0, 40, 40}), box(5, {64, 64, 96, 96})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    auto matches = match(owner, 0, {box(0, {90, 0, 120, 30}, .1F), box(0, {0, 0, 40, 40}, .9F), box(5, {64, 64, 96, 96}, .8F)}, 1);
    REQUIRE(matches.bbox.size() == 2U);  // Per-category cap, after ordering.
    CHECK(matches.bbox[0].prediction_ordinal == 1U);
    CHECK((matches.bbox[0].area_matched_bits[1] & 7U) == 7U);
    CHECK((matches.bbox[0].area_ignored_bits[1] & 7U) == 0U);
    CHECK((matches.bbox[0].area_ignored_bits[1] & 8U) != 0U);
    owner.merge_matches(std::move(matches));
    const auto summary = owner.evaluate(1, r::EvaluationDetailRetention::Detailed);
    REQUIRE(summary.bbox.average_recall[0]);
    CHECK(*summary.bbox.average_recall[0] == Approx(.75));
    auto details = owner.take_details();
    CHECK(row(details, r::EvaluationArea::All, 5U).average_precision[0] == Approx(1.0));
}
TEST_CASE("evaluation preserves COCO GT ties and upstream float64 confidence boundaries", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 20, 20}), box(0, {10, 0, 30, 20})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {5, 0, 25, 20}, .9F), box(0, {0, 0, 20, 20}, .8F)}, 10));
    auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.confidence.recall == Approx(1.0));
    for (const auto [score, threshold] : std::array<std::pair<float, double>, 4>{{{.60F, .61}, {.58F, .58}, {.5F, .51}, {0.0F, .01}}}) {
        owner.clear_predictions();
        owner.merge_matches(match(owner, 0,
                                  {box(0, {0, 0, 20, 20}, 1.0F), box(0, {10, 0, 30, 20}, 1.0F), box(0, {0, 0, 20, 20}, score),
                                   box(0, {0, 0, 20, 20}, std::numeric_limits<float>::quiet_NaN())},
                                  10));
        summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
        CHECK(summary.bbox.confidence_threshold == Approx(threshold));
        CHECK(summary.bbox.confidence.f1 == Approx(1.0));
    }
}
TEST_CASE("evaluation macro F1 averages per-class F1 rather than pooled counts", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 20, 20}), box(5, {32, 0, 52, 20}), box(5, {64, 0, 84, 20}), box(5, {96, 0, 116, 20})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 20, 20}), box(5, {32, 0, 52, 20}), box(5, {32, 0, 52, 20})}, 10));
    const auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.confidence_threshold == 0.0);
    CHECK(summary.bbox.confidence.precision == Approx(.75));
    CHECK(summary.bbox.confidence.recall == Approx(2.0 / 3.0));
    CHECK(summary.bbox.confidence.f1 == Approx(.7));
}
TEST_CASE("evaluation areas use original geometry and preserve the inclusive 96 squared boundary", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 96, 96})}}, 64);
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 48, 48})}, 10));
    const auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.ap == Approx(1.0));
    CHECK_FALSE(summary.bbox.area_ap[0]);
    REQUIRE(summary.bbox.area_ap[1]);
    CHECK(*summary.bbox.area_ap[1] == Approx(1.0));
    REQUIRE(summary.bbox.area_ap[2]);
    CHECK(*summary.bbox.area_ap[2] == Approx(1.0));
    CHECK_THROWS_AS(fixture.loader->geometry(1U), std::out_of_range);
    CHECK_THROWS_AS(match(owner, 1, {}, 10), std::out_of_range);
    CHECK_THROWS_AS(owner.evaluate(0U, r::EvaluationDetailRetention::Detailed), std::invalid_argument);
}
TEST_CASE("evaluation recall caps share the largest per-image ordered matching", "[rfdetr][evaluation][gpu]") {
    std::vector<r::Prediction> predictions;
    for (int index = 0; index < 12; ++index) {
        const float x = static_cast<float>(index % 6) * 20.0F;
        const float y = static_cast<float>(index / 6) * 20.0F;
        predictions.push_back(box(0, {x, y, x + 10.0F, y + 10.0F}));
    }
    EvaluationFixture fixture({predictions});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, predictions, 12));
    const auto summary = owner.evaluate(12, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.ap == Approx(1.0));
    CHECK(summary.bbox.detection_limits == std::array<std::uint32_t, 3>{1U, 10U, 12U});
    REQUIRE(summary.bbox.average_recall[0]);
    CHECK(*summary.bbox.average_recall[0] == Approx(1.0 / 12.0));
    REQUIRE(summary.bbox.average_recall[1]);
    CHECK(*summary.bbox.average_recall[1] == Approx(10.0 / 12.0));
    REQUIRE(summary.bbox.average_recall[2]);
    CHECK(*summary.bbox.average_recall[2] == Approx(1.0));
}
TEST_CASE("compact and detailed evaluation share all summary values and retain one ordered catalog", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32}), box(5, {32, 32, 96, 96})}, {}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBoxAndMask);
    const auto catalog = owner.class_catalog();
    auto names = std::vector<std::string>(catalog->names().rbegin(), catalog->names().rend());
    auto selected = std::make_shared<const data::catalog::ClassCatalog>(names);
    const auto permutation = selected->permutation_to(*catalog);
    CHECK(permutation.front() == catalog->size() - 1U);
    CHECK(permutation.back() == 0U);
    auto predictions = std::vector{box(static_cast<int>(permutation.back()), {0, 0, 32, 32}, .9F),
                                   box(static_cast<int>(permutation.front()), {32, 32, 96, 96}, .7F), box(0, {0, 0, 32, 32}, .7F)};
    owner.merge_matches(match(owner, 0, predictions, 10U, true));
    // Input replacement and loader destruction do not relabel the evaluated run.
    selected = std::make_shared<const data::catalog::ClassCatalog>(std::vector<std::string>{"later input"});
    fixture.loader.reset();
    CHECK(owner.class_catalog() == catalog);
    CHECK(owner.class_catalog()->names().front() == names.back());
    r::EvaluationDatasetOwner copied(owner);
    CHECK(copied.class_catalog() == catalog);
    for (const auto limit : {2U, 1U, 0U}) {
        owner.limit_images(limit);
        const auto compact = owner.evaluate(10U, r::EvaluationDetailRetention::CompactOnly);
        CHECK(owner.take_details().empty());
        const auto detailed = owner.evaluate(10U, r::EvaluationDetailRetention::Detailed);
        const auto compact_wire = mmltk::frameworks::serialization::reflected_value(compact);
        const auto detailed_wire = mmltk::frameworks::serialization::reflected_value(detailed);
        REQUIRE(compact_wire);
        REQUIRE(detailed_wire);
        CHECK(*compact_wire == *detailed_wire);
        const auto rows = owner.take_details();
        REQUIRE(rows.size() == (catalog->size() + 1U) * r::kEvaluationAreaCount * 2U);
        const auto& aggregate = row(rows, r::EvaluationArea::All, std::nullopt);
        CHECK_FALSE(aggregate.category_name);
        CHECK(aggregate.available == (limit != 0U));
        for (const auto& detail : rows) CHECK_FALSE(detail.category_name);  // Names are projected only on page copies.
    }
    CHECK(copied.image_ids() == std::vector<int>{1, 2});
    CHECK(copied.evaluate(10U, r::EvaluationDetailRetention::CompactOnly).bbox.available);
}
TEST_CASE("evaluation axes exactly preserve thresholds and curve extents", "[rfdetr][evaluation]") {
    constexpr std::array<double, 10> expected_iou{0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.95};
    STATIC_REQUIRE(r::EvaluationAxisCatalog::valid());
    CHECK(r::kEvaluationAxes.iou == expected_iou);
    const r::EvaluationMetricDetail detail;
    CHECK(detail.average_precision.size() == r::kEvaluationAxes.iou.size());
    CHECK(detail.average_recall[0].size() == r::kEvaluationAxes.iou.size());
    CHECK(detail.precision_curve.size() == r::kEvaluationAxes.iou.size());
    CHECK(detail.precision_curve[0].size() == r::kEvaluationAxes.recall.size());
    for (std::size_t index = 0U; index < r::kEvaluationAxes.recall.size(); ++index) {
        CHECK(r::kEvaluationAxes.recall[index] == static_cast<double>(index) * 0.01);
        CHECK(r::kEvaluationAxes.confidence[index] == static_cast<double>(index) * 0.01);
    }
}
TEST_CASE("each native double IoU threshold is inclusive at its exact rational boundary", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 20, 20})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    for (std::size_t index = 0U; index < r::kEvaluationIouCount; ++index) {
        const auto matches = match(owner, 0, {box(0, {0, 0, static_cast<float>(10U + index), 20})}, 1U);
        REQUIRE(matches.bbox.size() == 1U);
        CHECK(matches.bbox.front().area_matched_bits[0] == static_cast<std::uint16_t>((1U << (index + 1U)) - 1U));
    }
}
TEST_CASE("COCO crowds repeat ignore matches while ordinary annotations retain priority", "[rfdetr][evaluation][gpu]") {
    const auto crowd = box(0, {0, 0, 128, 128});
    const auto ordinary = box(0, {10, 10, 30, 30});
    EvaluationFixture fixture({{crowd, ordinary, box(1, {0, 0, 128, 128})}}, 128,
                              {{{{"iscrowd", true}}, {{"ignore", true}}, {{"iscrowd", true}, {"ignore", false}}}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBoxAndMask);
    CHECK(fixture.loader->label_data()[1].raw_ignore());
    const auto prediction = box(0, {10, 10, 28, 30}, .9F);
    auto matches = match(owner, 0, {prediction, prediction, prediction, box(1, {4, 4, 8, 8})}, 10, true);
    REQUIRE(matches.bbox.size() == 4U);
    REQUIRE(matches.mask);
    for (const auto* records : {&matches.bbox, &*matches.mask}) {
        // Ordinary IoU=.9 takes precedence over crowd overlap=1 through .9;
        // the crowd absorbs the .95 threshold and every subsequent detection.
        CHECK((*records)[0].area_matched_bits[0] == 1023U);
        CHECK((*records)[0].area_ignored_bits[0] == 512U);
        CHECK((*records)[1].area_ignored_bits[0] == 1023U);
        CHECK((*records)[2].area_ignored_bits[0] == 1023U);
        CHECK((*records)[3].area_ignored_bits[0] == 1023U);
    }
    owner.merge_matches(std::move(matches));
    const auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.ap == Approx(.9));
    CHECK(summary.mask->ap == Approx(.9));
    const auto details = owner.take_details();
    CHECK(row(details, r::EvaluationArea::All, 0U).ground_truth_count == 1U);
    CHECK_FALSE(row(details, r::EvaluationArea::All, 1U).available);
    CHECK_FALSE(row(details, r::EvaluationArea::All, 1U, r::EvaluationMetricKind::Mask).available);
}
TEST_CASE("evaluation retains duplicate fractional boxes and declared source area", "[rfdetr][evaluation][gpu]") {
    const auto annotation = box(0, {10.25F, 11.5F, 20.75F, 22.25F});
    EvaluationFixture fixture({{annotation, annotation}}, 64, {{{{"area", 9216.0}}, {{"area", 9216.0}}}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBoxAndMask);
    CHECK(owner.facts().ground_truth_count == 2U);
    auto prediction = annotation;
    for (auto& coordinate : prediction.bbox_xyxy) coordinate *= .5F;
    // Use the compiled categorical mask so this case isolates stored area.
    prediction.mask = {};
    prediction.mask.width = prediction.mask.height = 64;
    const auto& packed = fixture.loader->label_data()[0];
    const auto* runs = fixture.loader->rle_data() + packed.mask_rle_offset / sizeof(data::RLEPair);
    for (std::size_t i = 0; i < packed.mask_rle_pairs; ++i) {
        prediction.mask.runs.emplace_back(runs[i].start, runs[i].length);
        prediction.mask.area += runs[i].length;
    }
    owner.merge_matches(match(owner, 0, {prediction, prediction}, 10, true));
    const auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.ap == Approx(1));
    CHECK(summary.mask->ap == Approx(1));
    const auto details = owner.take_details();
    for (const auto kind : {r::EvaluationMetricKind::Box, r::EvaluationMetricKind::Mask}) {
        CHECK_FALSE(row(details, r::EvaluationArea::Small, 0U, kind).available);
        CHECK(row(details, r::EvaluationArea::Medium, 0U, kind).ground_truth_count == 2U);
        CHECK(row(details, r::EvaluationArea::Large, 0U, kind).ground_truth_count == 2U);
    }
}
TEST_CASE("known empty evaluation masks remain positive annotations and missing masks fail admission", "[rfdetr][evaluation][gpu]") {
    auto annotation = box(0, {1.25F, 2.5F, 7.75F, 8.5F});
    annotation.mask.runs.clear();
    annotation.mask.area = 0;
    EvaluationFixture fixture({{annotation}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBoxAndMask);
    CHECK(owner.facts().mask_rle_pair_count == 0U);
    CHECK(r::resolve_evaluation_metric_set(*fixture.loader, true) == r::EvaluationMetricSet::BBoxAndMask);
    owner.merge_matches(match(owner, 0, {annotation}, 10, true));
    const auto summary = owner.evaluate(10, r::EvaluationDetailRetention::Detailed);
    CHECK(summary.bbox.ap == Approx(1));
    REQUIRE(summary.mask);
    CHECK(summary.mask->available);
    CHECK(summary.mask->ap == 0);
    annotation.has_mask = false;
    EvaluationFixture missing({{annotation}});
    CHECK(r::resolve_evaluation_metric_set(*missing.loader, true) == r::EvaluationMetricSet::BBox);
    CHECK_THROWS_AS(r::EvaluationDatasetOwner(*missing.loader, r::EvaluationMetricSet::BBoxAndMask), std::runtime_error);
}
