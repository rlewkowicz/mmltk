#pragma once

#include "gui/annotation/tools/tool_manager.h"

#include <memory>

namespace mmltk::gui {

std::unique_ptr<AnnotationTool> make_annotation_tool(AnnotationToolKind kind);
void register_default_annotation_tools(AnnotationToolManager& manager);

}  
