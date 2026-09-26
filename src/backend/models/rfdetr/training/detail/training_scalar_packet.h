#pragma once
#include <cmath>
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/backend/models/rfdetr/core/reflected_tensor_packet.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingScalarPacket final : ReflectedTensorPacket<TrainingScalars> {
 static TrainingScalars project(const float* values, double count) {
  TrainingScalars result;
  visit([&]<auto Member, std::size_t Index>() {
   const auto value = static_cast<double>(values[Index]);
   if (count > 0 && std::isfinite(value)) result.*Member = value / count;
  });
  return result;
 }
};
}  // namespace mmltk::backend::models::rfdetr
