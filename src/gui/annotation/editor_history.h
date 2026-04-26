#pragma once

#include "gui/annotation/document/document.h"
#include "gui/annotation/session.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <string_view>

namespace mmltk::gui {

class AnnotationEditorTransactionScope {
   public:
    AnnotationEditorTransactionScope(AnnotationDocument& document, bool begin_if_needed) noexcept;
    AnnotationEditorTransactionScope(const AnnotationEditorTransactionScope&) = delete;
    AnnotationEditorTransactionScope& operator=(const AnnotationEditorTransactionScope&) = delete;
    ~AnnotationEditorTransactionScope();

    void finish() noexcept;

   private:
    AnnotationDocument* document_ = nullptr;
    bool owns_transaction_ = false;
    bool finished_ = false;
};

struct AnnotationSelectedObjectContext {
    std::size_t index = 0U;
    AnnotationObject object;
};

[[nodiscard]] std::optional<std::size_t> normalize_selected_object_index(const AnnotationDocument& document,
                                                                         const AnnotationSession& session) noexcept;
[[nodiscard]] const AnnotationObject* selected_object(const AnnotationDocument& document,
                                                      const AnnotationSession& session) noexcept;
void select_object(AnnotationSession& session, AnnotationDocument& document, std::optional<std::size_t> index) noexcept;

[[nodiscard]] std::optional<AnnotationSelectedObjectContext> selected_object_context(const AnnotationDocument& document,
                                                                                     const AnnotationSession& session);
[[nodiscard]] std::string selected_object_id_for_history(const AnnotationDocument& document,
                                                         const AnnotationSession& session);

template <typename Fn>
bool apply_selected_object_edit(AnnotationDocument& document, const AnnotationSession& session, Fn&& fn) {
    const std::optional<AnnotationSelectedObjectContext> context = selected_object_context(document, session);
    if (!context.has_value()) {
        return false;
    }
    return std::forward<Fn>(fn)(context->index, context->object);
}

[[nodiscard]] std::optional<std::size_t> clamp_selection_index(const AnnotationDocument& document,
                                                               std::optional<std::size_t> index) noexcept;
void repair_selection_after_history_jump(AnnotationDocument& document, AnnotationSession& session,
                                         std::optional<std::size_t> previous_index,
                                         std::string_view previous_object_id);

[[nodiscard]] bool begin_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                                      AnnotationEditTransactionKind kind, std::optional<std::size_t> object_index,
                                      std::optional<std::size_t> restore_selection_index);
[[nodiscard]] bool commit_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                                       AnnotationEditTransactionKind kind);
[[nodiscard]] bool cancel_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                                       AnnotationEditTransactionKind kind,
                                       std::optional<std::size_t> selection_after_cancel = std::nullopt);
[[nodiscard]] bool cancel_active_grouped_edit(AnnotationDocument& document, AnnotationSession& session,
                                              std::optional<std::size_t> selection_after_cancel = std::nullopt);
[[nodiscard]] bool cancel_grouped_edit_for_object_index(
    AnnotationDocument& document, AnnotationSession& session, std::size_t object_index,
    std::optional<std::size_t> selection_after_cancel = std::nullopt);
[[nodiscard]] bool cancel_grouped_edit_for_selection(AnnotationDocument& document, AnnotationSession& session,
                                                     std::optional<std::size_t> selection_after_cancel);
[[nodiscard]] bool should_group_transaction_for_change_count(std::size_t change_count) noexcept;

template <typename Fn>
bool apply_selected_object_transaction(AnnotationDocument& document, Fn&& fn) {
    AnnotationEditorTransactionScope transaction(document, true);
    const bool changed = std::forward<Fn>(fn)();
    transaction.finish();
    return changed;
}

}  // namespace mmltk::gui
