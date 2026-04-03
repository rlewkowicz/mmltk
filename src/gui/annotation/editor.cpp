#include "gui/annotation/editor.h"

#include <algorithm>
#include <string>
#include <utility>

namespace mmltk::gui {

bool AnnotationEditor::insert_object(AnnotationDocument& document,
                                     AnnotationSession& session,
                                     AnnotationObject object,
                                     const std::optional<std::size_t> index) {
    const std::optional<std::size_t> previous_selection =
        normalize_selected_object_index(document, session);
    const std::size_t insert_index =
        std::min(index.value_or(document.size()), document.size());
    if (!document.apply(AnnotationInsertObjectCommand{
            std::move(object),
            std::optional<std::size_t>{insert_index},
        })) {
        return false;
    }
    if (previous_selection.has_value() && insert_index <= *previous_selection) {
        session.select_object(*previous_selection + 1U);
    }
    return true;
}

bool AnnotationEditor::replace_object(AnnotationDocument& document,
                                      const std::size_t index,
                                      AnnotationObject object) {
    return document.apply(AnnotationReplaceObjectCommand{
        index,
        std::move(object),
    });
}

bool AnnotationEditor::remove_object(AnnotationDocument& document,
                                     AnnotationSession& session,
                                     const std::size_t index) {
    if (cancel_grouped_edit_for_object_index(document, session, index)) {
        return true;
    }
    const std::optional<std::size_t> previous_selection =
        normalize_selected_object_index(document, session);
    if (!document.apply(AnnotationRemoveObjectCommand{index})) {
        return false;
    }
    if (!previous_selection.has_value()) {
        return true;
    }
    if (document.empty()) {
        select_object(session, document, std::nullopt);
        return true;
    }
    if (*previous_selection == index || *previous_selection >= document.size()) {
        select_object(session,
                      document,
                      std::min(index, document.size() - 1U));
    } else if (*previous_selection > index) {
        session.select_object(*previous_selection - 1U);
    }
    return true;
}

bool AnnotationEditor::remove_selected_object(AnnotationDocument& document,
                                              AnnotationSession& session) {
    const std::optional<std::size_t> selected_object_index =
        normalize_selected_object_index(document, session);
    if (!selected_object_index.has_value()) {
        return false;
    }
    return remove_object(document, session, *selected_object_index);
}

bool AnnotationEditor::undo(AnnotationDocument& document,
                            AnnotationSession& session) {
    const std::optional<std::size_t> previous_index =
        normalize_selected_object_index(document, session);
    const std::string previous_object_id =
        selected_object_id_for_history(document, session);
    if (!document.undo()) {
        return false;
    }
    repair_selection_after_history_jump(document,
                                        session,
                                        previous_index,
                                        previous_object_id);
    return true;
}

bool AnnotationEditor::redo(AnnotationDocument& document,
                            AnnotationSession& session) {
    const std::optional<std::size_t> previous_index =
        normalize_selected_object_index(document, session);
    const std::string previous_object_id =
        selected_object_id_for_history(document, session);
    if (!document.redo()) {
        return false;
    }
    repair_selection_after_history_jump(document,
                                        session,
                                        previous_index,
                                        previous_object_id);
    return true;
}

bool AnnotationEditor::ensure_default_category(AnnotationCategories& categories,
                                               const std::string_view fallback_name) {
    if (!categories.items.empty()) {
        return false;
    }
    const std::string name = fallback_name.empty()
                                 ? std::string("object")
                                 : std::string(fallback_name);
    (void)ensure_annotation_category(categories, name);
    return true;
}

bool AnnotationEditor::add_category(AnnotationCategories& categories,
                                    const std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const std::size_t previous_count = categories.items.size();
    (void)ensure_annotation_category(categories, std::string(name));
    return categories.items.size() != previous_count;
}

bool AnnotationEditor::repair_object_category_indices(AnnotationDocument& document,
                                                      const AnnotationCategories& categories) {
    if (categories.items.empty()) {
        return false;
    }

    AnnotationEditorTransactionScope transaction(document, true);
    bool changed = false;
    for (std::size_t index = 0; index < document.size(); ++index) {
        const AnnotationObject* object = document.object(index);
        if (object == nullptr || object->category_index < categories.items.size()) {
            continue;
        }

        AnnotationObject updated = *object;
        updated.category_index = 0U;
        changed = document.apply(AnnotationReplaceObjectCommand{
                      index,
                      std::move(updated),
                  }) ||
                  changed;
    }
    transaction.finish();
    return changed;
}

bool AnnotationEditor::reset_selected_object_box(AnnotationDocument& document,
                                                 const AnnotationSession& session,
                                                 const AnnotationFrame& frame) {
    return apply_selected_object_edit(
        document,
        session,
        [&](const std::size_t selected_object_index, const AnnotationObject& object) {
            if (!annotation_object_supports_mask_editing(object)) {
                return false;
            }
            return apply_selected_object_transaction(document, [&]() {
                return document.apply(AnnotationSetObjectBoxCommand{
                    selected_object_index,
                    AnnotationBox{},
                    annotation_frame_capture_width(frame),
                    annotation_frame_capture_height(frame),
                });
            });
        });
}

bool AnnotationEditor::cleanup_selected_mask(AnnotationDocument& document,
                                             const AnnotationSession& session,
                                             const AnnotationFrame& frame,
                                             const AnnotationMaskCleanupOp op,
                                             const int radius) {
    return apply_selected_object_edit(
        document,
        session,
        [&](const std::size_t selected_object_index, const AnnotationObject&) {
            return apply_selected_object_transaction(document, [&]() {
                return document.apply(AnnotationCleanupMaskCommand{
                    selected_object_index,
                    op,
                    radius,
                    annotation_frame_capture_width(frame),
                    annotation_frame_capture_height(frame),
                });
            });
        });
}

bool AnnotationEditor::update_selected_object_color_ranges(
    AnnotationDocument& document,
    const AnnotationSession& session,
    const AnnotationColorRange& sup,
    const AnnotationColorRange& nosup) {
    return apply_selected_object_edit(document,
                                      session,
                                      [&](const std::size_t selected_object_index,
                                          const AnnotationObject&) {
                                          return apply_selected_object_transaction(document, [&]() {
                                              return document.apply(
                                                  AnnotationSetObjectColorRangesCommand{
                                                      selected_object_index,
                                                      sup,
                                                      nosup,
                                                  });
                                          });
                                      });
}

} // namespace mmltk::gui
