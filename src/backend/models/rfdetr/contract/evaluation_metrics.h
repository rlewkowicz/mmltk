#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"

namespace mmltk::backend::models::rfdetr {

enum class EvaluationMetricKind : std::uint8_t { Box, Mask };
enum class EvaluationArea : std::uint8_t { All, Small, Medium, Large };
MMLTK_REFLECT_ENUM(EvaluationMetricKind)
MMLTK_REFLECT_ENUM(EvaluationArea)

inline constexpr std::array<double, 10> kEvaluationIouThresholds{0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90, 0.95};
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
    std::array<std::optional<double>, 3> area_ap{};
    std::array<std::optional<double>, 3> area_ar{};
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
    bool available = false;
    std::uint64_t ground_truth_count = 0U;
    std::array<std::uint32_t, 3> detection_limits{};
    std::array<double, 10> average_precision{};
    std::array<std::array<double, 10>, 3> average_recall{};
    std::array<std::array<double, 101>, 10> precision_curve{};
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
