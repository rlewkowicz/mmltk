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
#include "filesystem_test_utils.hpp"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/dataset_loader.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
import mmltk.backend.models.rfdetr.core.evaluator;
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
    explicit EvaluationFixture(const std::vector<std::vector<r::Prediction>>& images, int resolution = 128)
        : root_("evaluator-answers") {
        const data::testsupport::FixtureSpec fixture{root_.path().string(), "train", 128, 128, static_cast<int>(images.size()), 0, 0};
        data::testsupport::create_synthetic_dataset(fixture);
        constexpr std::array names{"person", "ret", "scope", "iron_sight", "anchor_dot", "glint"};
        for (std::size_t image = 0; image < images.size(); ++image) {
            auto name = std::to_string(image + 1U); name.insert(0U, 6U - name.size(), '0');
            std::ofstream output(std::filesystem::path(data::testsupport::dataset_dir(fixture)) / "train" / (name + ".jsonl"));
            for (const auto& annotation : images[image]) {
                std::string runs;
                for (const auto& [start, count] : annotation.mask.runs) {
                    if (!runs.empty()) runs += ' ';
                    runs += std::to_string(start) + ':' + std::to_string(count);
                }
                output << nlohmann::json{{"class", names[annotation.class_reference]}, {"bbox_xyxy", annotation.bbox_xyxy},
                    {"mask_rle_encoding", "row_major_start_length"}, {"mask_rle", runs}, {"image_size_wh", {128, 128}}}.dump() << '\n';
            }
        }
        data::CompilerConfig config;
        config.source_dir = data::testsupport::dataset_dir(fixture); config.output_dir = data::testsupport::compiled_dir(fixture);
        config.split = "train"; config.target_width = resolution; config.target_height = resolution; config.num_workers = 1;
        const auto plan = data::DatasetCompiler::prepare(config, {config.split});
        data::DatasetCompiler::compile(plan, 0U);
        loader = std::make_unique<data::DatasetLoader>(data::DatasetLoader::Config{.compiled_path = data::testsupport::compiled_bin_path(fixture),
            .batch_size = 1U, .shuffle = false, .prefetch_factor = 2, .gather_workers = 1, .loading = data::data_loading_options(true)});
    }
    std::unique_ptr<data::DatasetLoader> loader;
 private:
    mmltk::testsupport::ScopedTempDir root_;
};
r::EvaluationDatasetOwner::ImageMatches match(r::EvaluationDatasetOwner& owner, std::int64_t image,
    const std::vector<r::Prediction>& predictions, std::size_t cap, bool masks = false) {
    std::vector<float> scores, boxes;
    std::vector<std::int64_t> labels;
    for (const auto& prediction : predictions) {
        scores.push_back(prediction.score); labels.push_back(prediction.class_reference);
        boxes.insert(boxes.end(), prediction.bbox_xyxy.begin(), prediction.bbox_xyxy.end());
    }
    return owner.match_predictions(image, {static_cast<int>(image + 1), scores.data(), labels.data(), boxes.data(), scores.size()},
        std::nullopt, cap, masks ? std::span<const r::Prediction>{predictions} : std::span<const r::Prediction>{});
}
const r::EvaluationMetricDetail& row(const std::vector<r::EvaluationMetricDetail>& rows, r::EvaluationArea area,
    std::optional<std::uint32_t> category, r::EvaluationMetricKind kind = r::EvaluationMetricKind::Box) {
    const auto found = std::ranges::find_if(rows, [&](const auto& value) { return value.area == area && value.category == category && value.kind == kind; });
    REQUIRE(found != rows.end());
    return *found;
}
}
TEST_CASE("evaluation distinguishes unavailable categories, measured zero, masks and empty populations", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32})}, {}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBoxAndMask);
    owner.merge_matches(match(owner, 0, {}, 10, true));
    auto summary = owner.evaluate(10);
    CHECK(summary.bbox.available); CHECK(summary.bbox.ap == 0.0);
    REQUIRE(summary.mask); CHECK(summary.mask->available); CHECK(summary.mask->ap == 0.0);
    auto details = owner.take_details();
    CHECK_FALSE(row(details, r::EvaluationArea::All, 5U).available);
    CHECK(row(details, r::EvaluationArea::Small, 0U).available);
    CHECK(row(details, r::EvaluationArea::Medium, 0U).available); // Inclusive 32² boundary.
    CHECK_FALSE(row(details, r::EvaluationArea::Large, 0U).available);
    owner.clear_predictions();
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 32, 32})}, 10, true));
    summary = owner.evaluate(10); CHECK(summary.bbox.ap == Approx(1.0)); CHECK(summary.mask->ap == Approx(1.0));
    owner.limit_images(0);
    summary = owner.evaluate(10); CHECK_FALSE(summary.bbox.available); CHECK(summary.bbox.confidence.f1 == 0.0);
    CHECK_FALSE(summary.bbox.average_recall[0]);
}
TEST_CASE("evaluation uses the 101-point envelope and all ten inclusive IoU thresholds", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32}), box(0, {64, 64, 96, 96})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 32, 32})}, 100));
    CHECK(owner.evaluate(100).bbox.ap == Approx(51.0 / 101.0));
    owner.clear_predictions();
    owner.merge_matches(match(owner, 0, {box(0, {96, 96, 128, 128}, 1.0F), box(0, {0, 0, 32, 32}, 0.9F)}, 100));
    CHECK(owner.evaluate(100).bbox.ap == Approx(51.0 / 202.0));
    owner.clear_predictions();
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 16, 32})}, 100));
    CHECK(owner.evaluate(100).bbox.ap == Approx(51.0 / 1010.0));
    auto details = owner.take_details();
    const auto& curve = row(details, r::EvaluationArea::All, 0U);
    CHECK(curve.average_precision[0] == Approx(51.0 / 101.0)); CHECK(curve.average_precision[1] == 0.0);
    CHECK(curve.precision_curve[0][50] == 1.0); CHECK(curve.precision_curve[0][51] == 0.0);
}
TEST_CASE("evaluation shares candidates but keeps independent area ignores and score-prefix caps", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 32, 32}), box(0, {0, 0, 40, 40}), box(5, {64, 64, 96, 96})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    auto matches = match(owner, 0, {box(0, {90, 0, 120, 30}, .1F), box(0, {0, 0, 40, 40}, .9F), box(5, {64, 64, 96, 96}, .8F)}, 1);
    REQUIRE(matches.bbox.size() == 2U); // Per-category cap, after ordering.
    CHECK(matches.bbox[0].prediction_ordinal == 1U);
    CHECK((matches.bbox[0].area_matched_bits[1] & 7U) == 7U);
    CHECK((matches.bbox[0].area_ignored_bits[1] & 7U) == 0U);
    CHECK((matches.bbox[0].area_ignored_bits[1] & 8U) != 0U);
    owner.merge_matches(std::move(matches));
    const auto summary = owner.evaluate(1);
    REQUIRE(summary.bbox.average_recall[0]); CHECK(*summary.bbox.average_recall[0] == Approx(.75));
    auto details = owner.take_details(); CHECK(row(details, r::EvaluationArea::All, 5U).average_precision[0] == Approx(1.0));
}
TEST_CASE("evaluation preserves COCO GT ties and upstream float64 confidence boundaries", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 20, 20}), box(0, {10, 0, 30, 20})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {5, 0, 25, 20}, .9F), box(0, {0, 0, 20, 20}, .8F)}, 10));
    auto summary = owner.evaluate(10); CHECK(summary.bbox.confidence.recall == Approx(1.0));
    for (const auto [score, threshold] : std::array<std::pair<float, double>, 4>{{{.60F, .61}, {.58F, .58}, {.5F, .51}, {0.0F, .01}}}) {
        owner.clear_predictions();
        owner.merge_matches(match(owner, 0, {box(0, {0, 0, 20, 20}, 1.0F), box(0, {10, 0, 30, 20}, 1.0F), box(0, {0, 0, 20, 20}, score),
            box(0, {0, 0, 20, 20}, std::numeric_limits<float>::quiet_NaN())}, 10));
        summary = owner.evaluate(10);
        CHECK(summary.bbox.confidence_threshold == Approx(threshold));
        CHECK(summary.bbox.confidence.f1 == Approx(1.0));
    }
}
TEST_CASE("evaluation macro F1 averages per-class F1 rather than pooled counts", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 20, 20}), box(5, {32, 0, 52, 20}), box(5, {64, 0, 84, 20}), box(5, {96, 0, 116, 20})}});
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 20, 20}), box(5, {32, 0, 52, 20}), box(5, {32, 0, 52, 20})}, 10));
    const auto summary = owner.evaluate(10);
    CHECK(summary.bbox.confidence_threshold == 0.0);
    CHECK(summary.bbox.confidence.precision == Approx(.75));
    CHECK(summary.bbox.confidence.recall == Approx(2.0 / 3.0));
    CHECK(summary.bbox.confidence.f1 == Approx(.7));
}

TEST_CASE("evaluation areas use original geometry and preserve the inclusive 96 squared boundary", "[rfdetr][evaluation][gpu]") {
    EvaluationFixture fixture({{box(0, {0, 0, 96, 96})}}, 64);
    r::EvaluationDatasetOwner owner(*fixture.loader, r::EvaluationMetricSet::BBox);
    owner.merge_matches(match(owner, 0, {box(0, {0, 0, 48, 48})}, 10));
    const auto summary = owner.evaluate(10);
    CHECK(summary.bbox.ap == Approx(1.0));
    CHECK_FALSE(summary.bbox.area_ap[0]);
    REQUIRE(summary.bbox.area_ap[1]); CHECK(*summary.bbox.area_ap[1] == Approx(1.0));
    REQUIRE(summary.bbox.area_ap[2]); CHECK(*summary.bbox.area_ap[2] == Approx(1.0));
    CHECK_THROWS_AS(fixture.loader->letterbox(1U), std::out_of_range);
    CHECK_THROWS_AS(match(owner, 1, {}, 10), std::out_of_range);
    CHECK_THROWS_AS(owner.evaluate(0U), std::invalid_argument);
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
    const auto summary = owner.evaluate(12);
    CHECK(summary.bbox.ap == Approx(1.0));
    CHECK(summary.bbox.detection_limits == std::array<std::uint32_t, 3>{1U, 10U, 12U});
    REQUIRE(summary.bbox.average_recall[0]); CHECK(*summary.bbox.average_recall[0] == Approx(1.0 / 12.0));
    REQUIRE(summary.bbox.average_recall[1]); CHECK(*summary.bbox.average_recall[1] == Approx(10.0 / 12.0));
    REQUIRE(summary.bbox.average_recall[2]); CHECK(*summary.bbox.average_recall[2] == Approx(1.0));
}
