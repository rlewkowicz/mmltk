#pragma once

#include "gui/annotation/tools/tool_manager.h"

#include <memory>

namespace mmltk::gui {

std::unique_ptr<AnnotationTool> make_spline_annotation_tool();

}
