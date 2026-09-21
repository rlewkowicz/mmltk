#pragma once
#include <string>
#include <vector>
#include "src/controller/contracts/annotation.h"
namespace mmltk::controller::browser::test_support {
inline contracts::AnnotationObject make_annotation_wire_object(const contracts::AnnotationPoint point, const contracts::AnnotationColor color) {
 using namespace contracts;
 return {
  .name = AnnotationText::From(std::string(kAnnotationNameCapacity, 'n')),
  .shape = AnnotationShape::Box,
  .box = {point, point},
  .point = point,
  .sup = {.center = color, .minus = color, .plus = color},
  .nosup = {.center = color, .minus = color, .plus = color},
  .mask_points = std::vector<AnnotationPoint>(kAnnotationGeometryCapacity, point),
  .spline_knots = std::vector<AnnotationSplineKnot>(kAnnotationGeometryCapacity,
                                                    {.point = point, .in = {.point = point, .enabled = true}, .out = {.point = point, .enabled = true}}),
  .skeleton_nodes =
   std::vector<AnnotationSkeletonNode>(kAnnotationGeometryCapacity, {.key = AnnotationText::From(std::string(kAnnotationNameCapacity, 'k')), .point = point}),
  .skeleton_edges = std::vector<AnnotationEdge>(kAnnotationGeometryCapacity, {.source = 6U, .target = 7U}),
 };
}
}  // namespace mmltk::controller::browser::test_support
