#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <meta>
#include <string_view>
#include "src/backend/data/catalog/class_catalog.h"
#include <vector>

#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"

namespace mmltk::backend::models::rfdetr {

enum class EvaluationMetricKind : std::uint8_t { Box, Mask };
enum class EvaluationArea : std::uint8_t { All, Small, Medium, Large };
MMLTK_REFLECT_ENUM(EvaluationMetricKind)
MMLTK_REFLECT_ENUM(EvaluationArea)

// One static numerical authority, projected by the existing catalog protocol.
struct EvaluationAxes final {
    std::string_view key;
    std::array<double, 10> iou;
    std::array<double, 101> recall{};
    std::array<double, 101> confidence{};
};
MMLTK_REFLECT_FIELDS(EvaluationAxes)
inline constexpr EvaluationAxes kEvaluationAxes = [] {
    EvaluationAxes axes{.key = "coco",
        .iou = {0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.95}};
    for (std::size_t index = 0; index < axes.recall.size(); ++index) axes.recall[index] = static_cast<double>(index) * 0.01;
    axes.confidence = axes.recall;
    return axes;
}();
inline constexpr auto& kEvaluationIouThresholds = kEvaluationAxes.iou;
inline constexpr std::size_t kEvaluationIouCount = kEvaluationAxes.iou.size();
inline constexpr std::size_t kEvaluationRecallCount = kEvaluationAxes.recall.size();
inline constexpr std::size_t kEvaluationConfidenceCount = kEvaluationAxes.confidence.size();
inline constexpr std::size_t kEvaluationAreaCount = std::meta::enumerators_of(^^EvaluationArea).size();
struct EvaluationAxisCatalog final {
    using row_type = EvaluationAxes;
    static constexpr std::string_view identity = "rfdetr.evaluation-axes";
    template <class Visitor> static constexpr void VisitRows(Visitor&& visitor) { visitor(kEvaluationAxes, 0U); }
    static constexpr std::string_view row_key(const row_type& row) noexcept { return row.key; }
    static consteval bool valid() noexcept {
        const auto ordered = [](const auto& values) {
            for (std::size_t index = 0U; index < values.size(); ++index)
                if (!(values[index] >= 0.0 && values[index] <= 1.0) || (index != 0U && !(values[index] > values[index - 1U]))) return false;
            return true;
        };
        std::size_t ordinal = 0U;
        for (const auto enumerator : std::meta::enumerators_of(^^EvaluationArea)) {
            if (static_cast<std::size_t>(std::meta::extract<EvaluationArea>(enumerator)) != ordinal++) return false;
        }
        return ordered(kEvaluationAxes.iou) && ordered(kEvaluationAxes.recall) && ordered(kEvaluationAxes.confidence)
            && kEvaluationIouCount == 10U && kEvaluationIouCount <= std::numeric_limits<std::uint16_t>::digits
            && kEvaluationAxes.iou.front() == 0.50 && kEvaluationAxes.iou[5] == 0.75 && kEvaluationAxes.iou.back() == 0.95
            && kEvaluationRecallCount == 101U && kEvaluationConfidenceCount == kEvaluationRecallCount
            && kEvaluationAxes.recall.front() == 0.0 && kEvaluationAxes.recall.back() == 1.0
            && kEvaluationAxes.confidence == kEvaluationAxes.recall && kEvaluationAreaCount == 4U;
    }
};
static_assert(EvaluationAxisCatalog::valid());
inline constexpr std::size_t kEvaluationDetailPageSize = 4U;

struct ConfidenceMetrics final {
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
};
MMLTK_REFLECT_FIELDS(ConfidenceMetrics)

// Compact epoch/snapshot facts. Detailed curves never enter training history.
struct MetricSummary final {
    double ap = 0.0;
    double ap50 = 0.0;
    double ap75 = 0.0;
    bool available = false;
    std::array<std::optional<double>, 3> average_recall{};
    std::array<std::uint32_t, 3> detection_limits{};
    std::array<std::optional<double>, kEvaluationAreaCount - 1U> area_ap{};
    std::array<std::optional<double>, kEvaluationAreaCount - 1U> area_ar{};
    ConfidenceMetrics confidence{};
    double confidence_threshold = 0.0;
};
MMLTK_REFLECT_FIELDS(MetricSummary)

struct EvalSummary final {
    MetricSummary bbox;
    std::optional<MetricSummary> mask;
    std::uint32_t model_detection_budget = 0U;
};
MMLTK_REFLECT_FIELDS(EvalSummary)

struct EvaluationMetricDetail final {
    EvaluationMetricKind kind = EvaluationMetricKind::Box;
    EvaluationArea area = EvaluationArea::All;
    // Empty means the macro aggregate; otherwise this is a foreground index.
    std::optional<std::uint32_t> category;
    std::optional<mmltk::backend::data::catalog::ClassName> category_name;
    bool available = false;
    std::uint64_t ground_truth_count = 0U;
    std::array<std::uint32_t, 3> detection_limits{};
    std::array<double, kEvaluationIouCount> average_precision{};
    std::array<std::array<double, kEvaluationIouCount>, 3> average_recall{};
    [[= mmltk::frameworks::reflection::CatalogProvider<EvaluationAxisCatalog>{}]]
    std::array<std::array<double, kEvaluationRecallCount>, kEvaluationIouCount> precision_curve{};
    ConfidenceMetrics confidence{};
    double confidence_threshold = 0.0;
};
MMLTK_REFLECT_FIELDS(EvaluationMetricDetail)

struct EvaluationDetailQuery final {
    std::uint64_t generation = 0U;
    std::uint32_t offset = 0U;
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{1U}]]
    [[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{kEvaluationDetailPageSize}]]
    std::uint32_t count = kEvaluationDetailPageSize;
};
MMLTK_REFLECT_FIELDS(EvaluationDetailQuery)

struct EvaluationDetailPage final {
    std::uint64_t generation = 0U;
    std::uint32_t total = 0U;
    std::uint32_t offset = 0U;
    [[= mmltk::frameworks::reflection::MaxItems{kEvaluationDetailPageSize}]]
    std::vector<EvaluationMetricDetail> rows;
};
MMLTK_REFLECT_FIELDS(EvaluationDetailPage)

}  // namespace mmltk::backend::models::rfdetr
