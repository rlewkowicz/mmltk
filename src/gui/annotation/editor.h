#pragma once

#include "gui/annotation/document/document.h"
#include "gui/annotation/editor_history.h"
#include "gui/annotation_core.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace mmltk::gui {

class AnnotationEditor {
   public:
    static bool insert_object(AnnotationDocument& document, AnnotationSession& session, AnnotationObject object,
                              std::optional<std::size_t> index = std::nullopt);
    static bool replace_object(AnnotationDocument& document, std::size_t index, AnnotationObject object);
    static bool remove_object(AnnotationDocument& document, AnnotationSession& session, std::size_t index);
    static bool remove_selected_object(AnnotationDocument& document, AnnotationSession& session);
    static bool undo(AnnotationDocument& document, AnnotationSession& session);
    static bool redo(AnnotationDocument& document, AnnotationSession& session);
    static bool ensure_default_category(AnnotationCategories& categories, std::string_view fallback_name = "object");
    static bool add_category(AnnotationCategories& categories, std::string_view name);
    static bool repair_object_category_indices(AnnotationDocument& document, const AnnotationCategories& categories);
    static bool reset_selected_object_box(AnnotationDocument& document, const AnnotationSession& session,
                                          const AnnotationFrame& frame);
    static bool cleanup_selected_mask(AnnotationDocument& document, const AnnotationSession& session,
                                      const AnnotationFrame& frame, AnnotationMaskCleanupOp op, int radius);
    static bool update_selected_object_color_ranges(AnnotationDocument& document, const AnnotationSession& session,
                                                    const AnnotationColorRange& sup, const AnnotationColorRange& nosup);
};

}  // namespace mmltk::gui
