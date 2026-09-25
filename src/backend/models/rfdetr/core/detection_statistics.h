#pragma once
#include "reflected_tensor_packet.h"
namespace mmltk::backend::models::rfdetr {
// Main-output sufficient statistics. Sum ranks for each logical microbatch
// before computing its class and cardinality ratios.
struct DetectionSufficientStatistics final {
 double class_error_sum = 0;
 double matched_count = 0;
 double cardinality_error_sum = 0;
 double image_count = 0;
};
using DetectionStatisticsPacket = ReflectedTensorPacket<DetectionSufficientStatistics>;
}  // namespace mmltk::backend::models::rfdetr
