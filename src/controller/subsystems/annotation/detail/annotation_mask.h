#pragma once
#include "src/controller/contracts/annotation.h"

namespace mmltk::controller::subsystems::annotation {
// Run storage stays normalized: sorted rows, disjoint inclusive intervals.
void normalize_mask(contracts::AnnotationObject&);
void stroke_mask(contracts::AnnotationObject&, contracts::AnnotationPoint, contracts::AnnotationPoint, std::uint16_t radius,
                 std::uint16_t width, std::uint16_t height, bool erase);
void cleanup_mask(contracts::AnnotationObject&, contracts::AnnotationMaskCleanup, std::uint16_t radius, std::uint16_t width,
                  std::uint16_t height);
void transform_mask(contracts::AnnotationObject&, contracts::AnnotationBox, contracts::AnnotationBox);
void fill_mask(contracts::AnnotationObject&, contracts::AnnotationPoint, std::uint16_t width, std::uint16_t height);
}  // namespace mmltk::controller::subsystems::annotation
