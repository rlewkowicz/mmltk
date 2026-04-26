#include "gui/annotation/editor_history.h"

#include <algorithm>
#include <string>

namespace mmltk::gui {

AnnotationEditorTransactionScope::AnnotationEditorTransactionScope(AnnotationDocument& document,
                                                                   const bool begin_if_needed) noexcept
    : document_(&document), owns_transaction_(begin_if_needed && !document.transaction_active()) {
    if (owns_transaction_) {
        document.begin_transaction();
    }
}

AnnotationEditorTransactionScope::~AnnotationEditorTransactionScope() {
    if (owns_transaction_ && !finished_ && document_ != nullptr) {
        document_->cancel_transaction();
    }
}

void AnnotationEditorTransactionScope::finish() noexcept {
    if (!owns_transaction_ || finished_ || document_ == nullptr) {
        return;
    }
    finished_ = true;
    if (!document_->commit_transaction()) {
        document_->cancel_transaction();
    }
}

namespace {

bool cancel_grouped_edit_if_kind_matches(AnnotationDocument& document, AnnotationSession& session,
                                         const AnnotationEditTransactionKind kind,
                                         const std::optional<std::size_t> selection_after_cancel) {
    const AnnotationGroupedEditTransactionState current_state = session.grouped_edit_transaction();
    if (current_state.kind != kind) {
        return false;
    }

    if (document.transaction_active()) {
        document.cancel_transaction();
    }
    std::optional<std::size_t> next_selection = selection_after_cancel;
    if (!next_selection.has_value()) {
        next_selection = current_state.restore_selection_index;
    }

    session.clear_grouped_edit_transaction();
    if (kind == AnnotationEditTransactionKind::SplineConstruction) {
        session.clear_spline_edit_state();
    } else if (kind == AnnotationEditTransactionKind::SkeletonConstruction) {
        session.clear_skeleton_edit_state();
    }
    session.select_object(clamp_selection_index(document, next_selection));
    return true;
}

}  // namespace

std::optional<std::size_t> normalize_selected_object_index(const AnnotationDocument& document,
                                                           const AnnotationSession& session) noexcept {
    const auto& selected_object_index = session.selected_object_index();
    if (!selected_object_index.has_value() || *selected_object_index >= document.size()) {
        return std::nullopt;
    }
    return selected_object_index;
}

const AnnotationObject* selected_object(const AnnotationDocument& document, const AnnotationSession& session) noexcept {
    const auto& selected_object_index = normalize_selected_object_index(document, session);
    return selected_object_index.has_value() ? document.object(*selected_object_index) : nullptr;
}

void select_object(AnnotationSession& session, AnnotationDocument& document,
                   std::optional<std::size_t> index) noexcept {
    if (index.has_value() && *index >= document.size()) {
        index.reset();
    }
    if (cancel_grouped_edit_for_selection(document, session, index) && index.has_value() && *index >= document.size()) {
        index.reset();
    }
    session.select_object(index);
}

std::optional<AnnotationSelectedObjectContext> selected_object_context(const AnnotationDocument& document,
                                                                       const AnnotationSession& session) {
    const auto& selected_object_index = normalize_selected_object_index(document, session);
    if (!selected_object_index.has_value()) {
        return std::nullopt;
    }
    const AnnotationObject* object = document.object(*selected_object_index);
    if (object == nullptr) {
        return std::nullopt;
    }
    return AnnotationSelectedObjectContext{
        *selected_object_index,
        *object,
    };
}

std::string selected_object_id_for_history(const AnnotationDocument& document, const AnnotationSession& session) {
    const auto& selected_object_index = normalize_selected_object_index(document, session);
    if (!selected_object_index.has_value()) {
        return {};
    }

    const AnnotationObject* object = document.object(*selected_object_index);
    if (object == nullptr) {
        return {};
    }
    return object->object_id;
}

std::optional<std::size_t> clamp_selection_index(const AnnotationDocument& document,
                                                 std::optional<std::size_t> index) noexcept {
    if (!index.has_value()) {
        return std::nullopt;
    }
    if (document.empty()) {
        return std::nullopt;
    }
    return std::min(*index, document.size() - 1U);
}

void repair_selection_after_history_jump(AnnotationDocument& document, AnnotationSession& session,
                                         const std::optional<std::size_t> previous_index,
                                         const std::string_view previous_object_id) {
    std::optional<std::size_t> next_index;
    if (!previous_object_id.empty()) {
        for (std::size_t index = 0; index < document.size(); ++index) {
            const AnnotationObject* object = document.object(index);
            if (object != nullptr && object->object_id == previous_object_id) {
                next_index = index;
                break;
            }
        }
    }
    if (!next_index.has_value() && previous_index.has_value() && !document.empty()) {
        next_index = std::min(*previous_index, document.size() - 1U);
    }
    select_object(session, document, next_index);
    session.clear_transient_state();
}

bool begin_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                        const AnnotationEditTransactionKind kind, const std::optional<std::size_t> object_index,
                        const std::optional<std::size_t> restore_selection_index) {
    if (kind == AnnotationEditTransactionKind::None) {
        return false;
    }

    const AnnotationGroupedEditTransactionState current_state = session.grouped_edit_transaction();
    if (current_state.kind == kind && current_state.object_index == object_index && document.transaction_active()) {
        return true;
    }
    if (current_state.kind != AnnotationEditTransactionKind::None || document.transaction_active()) {
        return false;
    }

    document.begin_transaction();
    session.set_grouped_edit_transaction(AnnotationGroupedEditTransactionState{
        kind,
        object_index,
        clamp_selection_index(document, restore_selection_index),
    });
    return true;
}

bool commit_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                         const AnnotationEditTransactionKind kind) {
    const AnnotationGroupedEditTransactionState current_state = session.grouped_edit_transaction();
    if (current_state.kind != kind) {
        return false;
    }

    const bool changed = document.transaction_active() ? document.commit_transaction() : false;
    session.clear_grouped_edit_transaction();
    return changed;
}

bool cancel_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                         const AnnotationEditTransactionKind kind, std::optional<std::size_t> selection_after_cancel) {
    return cancel_grouped_edit_if_kind_matches(document, session, kind, selection_after_cancel);
}

bool cancel_active_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                                std::optional<std::size_t> selection_after_cancel) {
    switch (session.grouped_edit_transaction().kind) {
        case AnnotationEditTransactionKind::SplineConstruction:
            return cancel_grouped_edit(document, session, AnnotationEditTransactionKind::SplineConstruction,
                                       selection_after_cancel);
        case AnnotationEditTransactionKind::SkeletonConstruction:
            return cancel_grouped_edit(document, session, AnnotationEditTransactionKind::SkeletonConstruction,
                                       selection_after_cancel);
        case AnnotationEditTransactionKind::None:
            break;
    }
    return false;
}

bool cancel_grouped_edit_for_object_index(AnnotationDocument& document, AnnotationSession& session,
                                          const std::size_t object_index,
                                          std::optional<std::size_t> selection_after_cancel) {
    const AnnotationGroupedEditTransactionState& grouped_edit = session.grouped_edit_transaction();
    if (grouped_edit.kind == AnnotationEditTransactionKind::None || !grouped_edit.object_index.has_value() ||
        *grouped_edit.object_index != object_index) {
        return false;
    }
    return cancel_active_grouped_edit(document, session, selection_after_cancel);
}

bool cancel_grouped_edit_for_selection(AnnotationDocument& document, AnnotationSession& session,
                                       const std::optional<std::size_t> selection_after_cancel) {
    const AnnotationGroupedEditTransactionState& grouped_edit = session.grouped_edit_transaction();
    if (grouped_edit.kind == AnnotationEditTransactionKind::None ||
        grouped_edit.object_index == selection_after_cancel) {
        return false;
    }
    return cancel_active_grouped_edit(document, session, selection_after_cancel);
}

bool should_group_transaction_for_change_count(const std::size_t change_count) noexcept {
    return change_count > 1U;
}

}  // namespace mmltk::gui
