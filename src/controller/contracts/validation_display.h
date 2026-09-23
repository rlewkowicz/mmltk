#pragma once
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
namespace mmltk::controller::contracts {
struct ValidationDisplaySettings final {
 [[= mmltk::frameworks::reflection::Minimum<float>{0.0F}]]
 [[= mmltk::frameworks::reflection::Maximum<float>{1.0F}]]
 [[= mmltk::frameworks::reflection::Finite{}]] float confidence_threshold = 0.4F;
 bool operator==(const ValidationDisplaySettings&) const = default;
};
MMLTK_REFLECT_FIELDS(ValidationDisplaySettings)
}  // namespace mmltk::controller::contracts
