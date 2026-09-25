#pragma once
#include <cmath>
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/backend/models/rfdetr/core/reflected_tensor_packet.h"
namespace mmltk::backend::models::rfdetr {
struct TrainingScalarPacket final : ReflectedTensorPacket<TrainingScalars> {
 static TrainingScalars project(const float* values, double count) {
  TrainingScalars result;
  template for (constexpr auto member : members) {
   const auto value = static_cast<double>(values[index<member>()]);
   if (count > 0 && std::isfinite(value)) result.[:member:] = value / count;
  }
  return result;
 }
};
}  // namespace mmltk::backend::models::rfdetr
