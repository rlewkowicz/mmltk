#pragma once
#include <cstddef>
#include <vector>
#include "src/controller/contracts/annotation.h"
namespace mmltk::controller {
[[nodiscard]] std::vector<contracts::AnnotationColor> annotation_class_palette(std::size_t);
}  // namespace mmltk::controller
