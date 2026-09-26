#pragma once
#include <cstdint>
#include "src/backend/models/rfdetr/contract/training_metrics.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingEpochMetricState final {
 std::uint64_t microbatches = 0;
 double loss_sum = 0;
 double class_loss_sum = 0;
 double box_loss_sum = 0;
 TrainingScalars scalar_sums;
 [[nodiscard]] TrainingScalars scalar_means() const {
  if (!microbatches) return {};
  auto result = scalar_sums;
  mmltk::frameworks::reflection::visit_materialized_members<TrainingScalars>([&]<class Declaration>(const auto&) {
   auto& value = result.*Declaration::pointer;
   if (value) *value /= static_cast<double>(microbatches);
  });
  return result;
 }
};
MMLTK_REFLECT_FIELDS(TrainingEpochMetricState)
}  // namespace mmltk::backend::models::rfdetr
