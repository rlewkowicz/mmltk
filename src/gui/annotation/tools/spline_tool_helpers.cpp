#include "gui/annotation/tools/spline_tool_helpers.h"

#include "gui/annotation/document/edit.h"
#include "gui/annotation/editor_history.h"
#include "gui/annotation/tools/tool_helpers.h"
#include "gui/annotation/workspace/model.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace mmltk::gui::tool_detail {

namespace {

std::size_t spline_segment_count(const AnnotationSplineShape& shape) noexcept {
    if (shape.knots.size() <= 1U) {
        return 0U;
    }
    return shape.closed ? shape.knots.size() : shape.knots.size() - 1U;
}

std::optional<std::size_t> clamp_spline_segment_index(const AnnotationSplineShape& shape,
                                                      std::optional<std::size_t> index) noexcept {
    const std::size_t segment_count = spline_segment_count(shape);
    if (!index.has_value() || *index >= segment_count) {
        return std::nullopt;
    }
    return index;
}

[[nodiscard]] AnnotationSplineEditState make_spline_edit_state(
    const std::size_t object_index,
    const AnnotationSplineShape& shape,
    const std::optional<std::size_t> active_knot_index,
    const std::optional<std::size_t> active_segment_index,
    const bool close_intent,
    const bool reopen_requested) noexcept {
    return AnnotationSplineEditState{
        object_index,
        clamp_active_index(shape.knots.size(), active_knot_index),
        clamp_spline_segment_index(shape, active_segment_index),
        close_intent,
        reopen_requested,
    };
}

/// Apply a document command, then re-read the SplineShape and call a
/// user-supplied callback to refresh the edit state.  Returns \c true when
/// the command changed the document.
template <typename ApplyFn, typename RefreshFn>
bool apply_and_refresh_spline_state(AnnotationDocument& document,
                                    AnnotationSession& session,
                                    const std::size_t selected_index,
                                    ApplyFn&& apply_fn,
                                    RefreshFn&& refresh_fn) {
    const bool changed = std::forward<ApplyFn>(apply_fn)();
    if (changed) {
        const AnnotationSplineShape* updated_spline =
            std::get_if<AnnotationSplineShape>(
                &document.object(selected_index)->shape);
        if (updated_spline != nullptr) {
            std::forward<RefreshFn>(refresh_fn)(*updated_spline);
        }
    }
    return changed;
}

[[nodiscard]] AnnotationSplineHandleMode next_spline_handle_mode(
    const AnnotationSplineHandleMode mode) noexcept {
    switch (mode) {
    case AnnotationSplineHandleMode::Corner:
        return AnnotationSplineHandleMode::Smooth;
    case AnnotationSplineHandleMode::Smooth:
        return AnnotationSplineHandleMode::Mirrored;
    case AnnotationSplineHandleMode::Mirrored:
        return AnnotationSplineHandleMode::Corner;
    }
    return AnnotationSplineHandleMode::Corner;
}

bool apply_selected_spline_close(AnnotationDocument& document,
                                 AnnotationSession& session,
                                 const std::size_t selected_index,
                                 const AnnotationSplineEditState& current_state) {
    return apply_and_refresh_spline_state(
        document, session, selected_index,
        [&] { return document.apply(AnnotationCloseSplineCommand{selected_index}); },
        [&](const AnnotationSplineShape& updated) {
            set_active_spline_edit_state(session, selected_index, updated,
                                         current_state.active_knot_index,
                                         current_state.active_segment_index,
                                         false, false);
        });
}

bool apply_selected_spline_reopen(AnnotationDocument& document,
                                  AnnotationSession& session,
                                  const std::size_t selected_index,
                                  const AnnotationSplineEditState& current_state) {
    return apply_and_refresh_spline_state(
        document, session, selected_index,
        [&] { return document.apply(AnnotationReopenSplineCommand{selected_index}); },
        [&](const AnnotationSplineShape& updated) {
            set_active_spline_edit_state(session, selected_index, updated,
                                         current_state.active_knot_index,
                                         current_state.active_segment_index,
                                         false, false);
        });
}

bool apply_selected_spline_delete_active_knot(AnnotationDocument& document,
                                              AnnotationSession& session,
                                              const std::size_t selected_index,
                                              const AnnotationSplineEditState& current_state) {
    if (!current_state.active_knot_index.has_value()) {
        return false;
    }
    return apply_and_refresh_spline_state(
        document, session, selected_index,
        [&] {
            return document.apply(AnnotationRemoveSplineKnotCommand{
                selected_index, *current_state.active_knot_index});
        },
        [&](const AnnotationSplineShape& updated) {
            set_active_spline_edit_state(session, selected_index, updated,
                                         current_state.active_knot_index);
        });
}

bool apply_selected_spline_cycle_handle_mode(AnnotationDocument& document,
                                             const std::size_t selected_index,
                                             const AnnotationSplineEditState& current_state,
                                             const AnnotationSplineShape& spline) {
    if (!current_state.active_knot_index.has_value()) {
        return false;
    }

    const AnnotationSplineHandleMode next_mode =
        next_spline_handle_mode(spline.knots[*current_state.active_knot_index].handle_mode);
    return document.apply(AnnotationSetSplineKnotHandleModeCommand{
        selected_index,
        *current_state.active_knot_index,
        next_mode,
    });
}

[[nodiscard]] AnnotationToolMutation make_spline_tool_mutation(const bool changed) {
    return AnnotationToolMutation{
        changed,
        false,
        std::nullopt,
        false,
    };
}

AnnotationToolMutation handle_spline_new_object_click(const AnnotationToolContext& context,
                                                      const AnnotationToolCanvasClickEvent& event,
                                                      const AnnotationPoint& point) {
    if (event.double_click) {
        return {};
    }

    const auto& restore_selection = context.session.selected_object_index();
    if (!begin_grouped_tool_edit(context,
                                 AnnotationToolKind::Spline,
                                 std::nullopt,
                                 restore_selection)) {
        return {};
    }

    AnnotationObject object = make_spline_annotation_object(context.document.size(),
                                                            event.default_category_index);
    if (!append_spline_knot(&object, point) ||
        !context.document.apply(AnnotationInsertObjectCommand{std::move(object), std::nullopt})) {
        (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
        return {};
    }

    const std::size_t inserted_index = context.document.size() - 1U;
    context.session.select_object(inserted_index);
    bind_grouped_tool_edit_object(context.session, inserted_index);
    finalize_created_spline_object(context.document,
                                   context.session,
                                   inserted_index,
                                   0U);

    return make_spline_tool_mutation(true);
}

AnnotationToolMutation handle_spline_existing_object_click(
    const AnnotationToolContext& context,
    const AnnotationToolCanvasClickEvent& event,
    const std::size_t selected_index,
    const AnnotationSplineEditState& current_state) {
    if (event.double_click || current_state.close_intent) {
        const bool changed = apply_and_refresh_spline_state(
            context.document, context.session, selected_index,
            [&] { return context.document.apply(AnnotationCloseSplineCommand{selected_index}); },
            [&](const AnnotationSplineShape& updated) {
                set_active_spline_edit_state(context.session, selected_index, updated,
                                             current_state.active_knot_index);
            });
        if (changed) {
            (void)commit_grouped_tool_edit(context, AnnotationToolKind::Spline);
        }
        return make_spline_tool_mutation(changed);
    }

    if (current_state.active_segment_index.has_value()) {
        if (!begin_grouped_tool_edit(context,
                                     AnnotationToolKind::Spline,
                                     selected_index,
                                     selected_index)) {
            return {};
        }
        const bool changed = apply_and_refresh_spline_state(
            context.document, context.session, selected_index,
            [&] {
                return context.document.apply(AnnotationInsertSplineKnotCommand{
                    selected_index, *current_state.active_segment_index,
                    static_cast<float>(event.capture_x)});
            },
            [&](const AnnotationSplineShape& updated) {
                set_active_spline_edit_state(context.session, selected_index, updated,
                                             *current_state.active_segment_index + 1U);
            });
        if (!changed) {
            (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
        }
        return make_spline_tool_mutation(changed);
    }

    if (!begin_grouped_tool_edit(context,
                                 AnnotationToolKind::Spline,
                                 selected_index,
                                 selected_index)) {
        return {};
    }
    const bool changed = apply_and_refresh_spline_state(
        context.document, context.session, selected_index,
        [&] {
            return context.document.apply(AnnotationAppendSplineKnotCommand{
                selected_index, static_cast<float>(event.capture_x)});
        },
        [&](const AnnotationSplineShape& updated) {
            set_active_spline_edit_state(context.session, selected_index, updated,
                                         updated.knots.size() - 1U);
        });
    if (!changed) {
        (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
    }
    return make_spline_tool_mutation(changed);
}

} // namespace

void seed_created_spline_edit_state(AnnotationSession& session,
                                    const std::size_t object_index,
                                    const AnnotationObject& object) noexcept {
    const AnnotationSplineShape* spline = std::get_if<AnnotationSplineShape>(&object.shape);
    if (spline == nullptr) {
        return;
    }
    set_active_spline_edit_state(session,
                                 object_index,
                                 *spline,
                                 std::nullopt,
                                 std::nullopt,
                                 false,
                                 false);
}

void finalize_created_spline_object(AnnotationDocument& document,
                                    AnnotationSession& session,
                                    const std::size_t object_index,
                                    const std::optional<std::size_t> active_knot_index) {
    const AnnotationObject* object = document.object(object_index);
    if (object == nullptr) {
        return;
    }
    const AnnotationSplineShape* spline = std::get_if<AnnotationSplineShape>(&object->shape);
    if (spline == nullptr) {
        return;
    }
    set_active_spline_edit_state(session,
                                 object_index,
                                 *spline,
                                 active_knot_index,
                                 std::nullopt,
                                 false,
                                 false);
}

AnnotationSplineEditState current_or_seed_spline_edit_state(const AnnotationSession& session,
                                                            const std::size_t object_index,
                                                            const AnnotationSplineShape& shape) noexcept {
    if (session.spline_edit_state().object_index == object_index) {
        const AnnotationSplineEditState& current_state = session.spline_edit_state();
        return make_spline_edit_state(object_index,
                                      shape,
                                      current_state.active_knot_index,
                                      current_state.active_segment_index,
                                      current_state.close_intent,
                                      current_state.reopen_requested);
    }
    return make_spline_edit_state(object_index,
                                  shape,
                                  shape.knots.empty()
                                      ? std::optional<std::size_t>{}
                                      : std::optional<std::size_t>{shape.knots.size() - 1U},
                                  std::nullopt,
                                  false,
                                  false);
}

AnnotationSidebarSelectedSplineViewModel make_selected_spline_sidebar_view_model(
    const AnnotationSession& session,
    const std::size_t object_index,
    const AnnotationSplineShape& shape) {
    const AnnotationSplineEditState edit_state =
        current_or_seed_spline_edit_state(session, object_index, shape);
    const AnnotationGroupedEditTransactionState grouped_edit =
        session.grouped_edit_transaction();
    const std::optional<std::size_t> active_knot_index = edit_state.active_knot_index;
    const std::optional<std::size_t> active_segment_index = edit_state.active_segment_index;
    return AnnotationSidebarSelectedSplineViewModel{
        shape.knots.size(),
        spline_segment_count(shape),
        shape.closed,
        active_knot_index,
        active_segment_index,
        active_knot_index.has_value() &&
                *active_knot_index < shape.knots.size()
            ? std::optional<AnnotationSplineHandleMode>{
                  shape.knots[*active_knot_index].handle_mode,
              }
            : std::nullopt,
        edit_state.close_intent,
        edit_state.reopen_requested,
        !shape.closed && shape.knots.size() >= 3U,
        shape.closed,
        active_segment_index.has_value(),
        active_knot_index.has_value(),
        active_knot_index.has_value(),
        grouped_edit.kind == AnnotationEditTransactionKind::SplineConstruction &&
            grouped_edit.object_index == object_index,
    };
}

void set_active_spline_edit_state(AnnotationSession& session,
                                  const std::size_t object_index,
                                  const AnnotationSplineShape& shape,
                                  const std::optional<std::size_t> active_knot_index,
                                  const std::optional<std::size_t> active_segment_index,
                                  const bool close_intent,
                                  const bool reopen_requested) noexcept {
    session.set_spline_edit_state(make_spline_edit_state(object_index,
                                                         shape,
                                                         active_knot_index,
                                                         active_segment_index,
                                                         close_intent,
                                                         reopen_requested));
}

AnnotationToolMutation handle_spline_canvas_click(const AnnotationToolContext& context,
                                                  const AnnotationToolCanvasClickEvent& event) {
    const AnnotationPoint point = capture_point_from_event(event);
    const auto& selected_index = context.session.selected_object_index();
    if (selected_index.has_value()) {
        const AnnotationObject* selected_object = context.document.object(*selected_index);
        if (selected_object != nullptr) {
            if (const auto* spline = std::get_if<AnnotationSplineShape>(&selected_object->shape);
                spline != nullptr && !spline->closed) {
                const AnnotationSplineEditState current_state =
                    current_or_seed_spline_edit_state(context.session, *selected_index, *spline);
                return handle_spline_existing_object_click(context,
                                                           event,
                                                           *selected_index,
                                                           current_state);
            }
        }
    }

    return handle_spline_new_object_click(context, event, point);
}

AnnotationToolMutation handle_spline_action(const AnnotationToolContext& context,
                                            const AnnotationToolActionEvent& event) {
    const auto& selected_index = context.session.selected_object_index();
    if (!selected_index.has_value()) {
        return {};
    }
    const AnnotationObject* selected_object = context.document.object(*selected_index);
    const AnnotationSplineShape* spline =
        selected_object != nullptr
            ? std::get_if<AnnotationSplineShape>(&selected_object->shape)
            : nullptr;
    if (spline == nullptr) {
        return {};
    }

    const AnnotationSplineEditState current_state =
        current_or_seed_spline_edit_state(context.session, *selected_index, *spline);

    switch (event.action) {
    case AnnotationToolActionKind::Confirm: {
        const bool changed = apply_selected_spline_close(context.document,
                                                         context.session,
                                                         *selected_index,
                                                         current_state);
        if (changed) {
            (void)commit_grouped_tool_edit(context, AnnotationToolKind::Spline);
        } else {
            (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
        }
        return make_spline_tool_mutation(changed);
    }
    case AnnotationToolActionKind::Cancel:
        if (cancel_grouped_tool_edit(context, AnnotationToolKind::Spline)) {
            return make_spline_tool_mutation(true);
        }
        context.session.clear_spline_edit_state();
        return {};
    case AnnotationToolActionKind::DeleteActiveElement:
        if (!current_state.active_knot_index.has_value()) {
            return {};
        }
        if (!begin_grouped_tool_edit(context,
                                     AnnotationToolKind::Spline,
                                     selected_index,
                                     selected_index)) {
            return {};
        }
        {
            const bool changed = apply_selected_spline_delete_active_knot(context.document,
                                                                          context.session,
                                                                          *selected_index,
                                                                          current_state);
            if (changed) {
                (void)commit_grouped_tool_edit(context, AnnotationToolKind::Spline);
            } else {
                (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
            }
            return make_spline_tool_mutation(changed);
        }
    case AnnotationToolActionKind::CycleHandleMode:
        if (!current_state.active_knot_index.has_value()) {
            return {};
        }
        if (!begin_grouped_tool_edit(context,
                                     AnnotationToolKind::Spline,
                                     selected_index,
                                     selected_index)) {
            return {};
        }
        {
            const bool changed = apply_selected_spline_cycle_handle_mode(context.document,
                                                                         *selected_index,
                                                                         current_state,
                                                                         *spline);
            if (!changed) {
                (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
                return {};
            }
            (void)commit_grouped_tool_edit(context, AnnotationToolKind::Spline);
            return make_spline_tool_mutation(true);
        }
    case AnnotationToolActionKind::ReopenSpline: {
        if (!begin_grouped_tool_edit(context,
                                     AnnotationToolKind::Spline,
                                     selected_index,
                                     selected_index)) {
            return {};
        }
        const bool changed = apply_selected_spline_reopen(context.document,
                                                          context.session,
                                                          *selected_index,
                                                          current_state);
        if (changed) {
            (void)commit_grouped_tool_edit(context, AnnotationToolKind::Spline);
        } else {
            (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Spline);
        }
        return make_spline_tool_mutation(changed);
    }
    case AnnotationToolActionKind::SkipJoint:
    case AnnotationToolActionKind::HideJoint:
    case AnnotationToolActionKind::ReactivateJoint:
    case AnnotationToolActionKind::ReseedJoint:
        break;
    }
    return {};
}

bool insert_selected_spline_knot_at_active_segment(AnnotationDocument& document,
                                                   AnnotationSession& session) {
    return apply_selected_object_edit(document,
                                      session,
                                      [&document, &session](
                                          const std::size_t selected_object_index,
                                          const AnnotationObject& object) {
                                          const AnnotationSplineShape* spline =
                                              std::get_if<AnnotationSplineShape>(&object.shape);
                                          if (spline == nullptr) {
                                              return false;
                                          }

                                          const AnnotationSplineEditState current_state =
                                              current_or_seed_spline_edit_state(session,
                                                                                selected_object_index,
                                                                                *spline);
                                          if (!current_state.active_segment_index.has_value()) {
                                              return false;
                                          }

                                          const std::optional<AnnotationPoint> insert_point =
                                              annotation_spline_segment_point(
                                                  *spline,
                                                  *current_state.active_segment_index,
                                                  0.5f);
                                          if (!insert_point.has_value()) {
                                              return false;
                                          }

                                          return apply_and_refresh_spline_state(
                                              document, session, selected_object_index,
                                              [&] {
                                                  return document.apply(
                                                      AnnotationInsertSplineKnotCommand{
                                                          selected_object_index,
                                                          *current_state.active_segment_index,
                                                          *insert_point});
                                              },
                                              [&](const AnnotationSplineShape& updated) {
                                                  set_active_spline_edit_state(
                                                      session, selected_object_index, updated,
                                                      *current_state.active_segment_index + 1U);
                                              });
                                      });
}

bool set_selected_spline_active_segment(AnnotationDocument& document,
                                        AnnotationSession& session,
                                        const std::optional<std::size_t> segment_index) {
    return apply_selected_object_edit(document,
                                      session,
                                      [&session, segment_index](
                                          const std::size_t selected_object_index,
                                          const AnnotationObject& object) {
                                          const AnnotationSplineShape* spline =
                                              std::get_if<AnnotationSplineShape>(&object.shape);
                                          if (spline == nullptr) {
                                              return false;
                                          }

                                          const std::size_t segment_count =
                                              spline_segment_count(*spline);
                                          if (segment_index.has_value() &&
                                              *segment_index >= segment_count) {
                                              return false;
                                          }

                                          const AnnotationSplineEditState current_state =
                                              current_or_seed_spline_edit_state(session,
                                                                                selected_object_index,
                                                                                *spline);
                                          const AnnotationSplineEditState next_state{
                                              selected_object_index,
                                              current_state.active_knot_index,
                                              segment_index,
                                              current_state.close_intent,
                                              current_state.reopen_requested,
                                          };
                                          if (current_state.object_index == next_state.object_index &&
                                              current_state.active_knot_index ==
                                                  next_state.active_knot_index &&
                                              current_state.active_segment_index ==
                                                  next_state.active_segment_index &&
                                              current_state.close_intent ==
                                                  next_state.close_intent &&
                                              current_state.reopen_requested ==
                                                  next_state.reopen_requested) {
                                              return false;
                                          }

                                          session.set_spline_edit_state(next_state);
                                          return true;
                                      });
}

bool set_selected_spline_handle_mode(AnnotationDocument& document,
                                     const AnnotationSession& session,
                                     const AnnotationSplineHandleMode mode) {
    return apply_selected_object_edit(document,
                                      session,
                                      [&document, &session, mode](
                                          const std::size_t selected_object_index,
                                          const AnnotationObject& object) {
                                          const AnnotationSplineShape* spline =
                                              std::get_if<AnnotationSplineShape>(&object.shape);
                                          if (spline == nullptr || spline->knots.empty()) {
                                              return false;
                                          }

                                          std::optional<std::size_t> knot_index =
                                              session.spline_edit_state().object_index ==
                                                      selected_object_index
                                                  ? session.spline_edit_state().active_knot_index
                                                  : std::nullopt;
                                          if (!knot_index.has_value() ||
                                              *knot_index >= spline->knots.size()) {
                                              knot_index = spline->knots.size() - 1U;
                                          }

                                          return document.apply(
                                              AnnotationSetSplineKnotHandleModeCommand{
                                                  selected_object_index,
                                                  *knot_index,
                                                  mode,
                                              });
                                      });
}

AnnotationSplineSidebarMutationResult apply_annotation_spline_sidebar_edit(
    AnnotationDocument& document,
    AnnotationSession& session,
    const AnnotationFrame* frame,
    const AnnotationCategories& categories,
    const AnnotationSplineSidebarEditRequest& request) {
    if (!request.request_insert_active_spline_knot &&
        !request.update_spline_active_segment &&
        !request.spline_handle_mode.has_value() &&
        !request.spline_action.has_value()) {
        return {};
    }

    AnnotationSplineSidebarMutationResult result;
    {
        const bool has_direct_sidebar_edit =
            request.request_insert_active_spline_knot ||
            request.update_spline_active_segment ||
            request.spline_handle_mode.has_value();
        AnnotationEditorTransactionScope transaction(document,
                                                     has_direct_sidebar_edit);

        if (request.request_insert_active_spline_knot &&
            insert_selected_spline_knot_at_active_segment(document, session)) {
            result.preview_invalidated = true;
            result.next_tool = AnnotationToolKind::Spline;
        }

        if (request.update_spline_active_segment &&
            set_selected_spline_active_segment(document,
                                               session,
                                               request.spline_active_segment_index)) {
            result.next_tool = AnnotationToolKind::Spline;
        }

        if (request.spline_handle_mode.has_value() &&
            set_selected_spline_handle_mode(document, session, *request.spline_handle_mode)) {
            result.preview_invalidated = true;
            result.next_tool = AnnotationToolKind::Spline;
        }

        transaction.finish();
    }

    if (request.spline_action.has_value() && frame != nullptr) {
        const AnnotationToolMutation mutation =
            handle_spline_action(AnnotationToolContext{
                                     document,
                                     session,
                                     *frame,
                                     categories,
                                     annotation_frame_capture_width(*frame),
                                     annotation_frame_capture_height(*frame),
                                 },
                                 AnnotationToolActionEvent{*request.spline_action});
        if (mutation.selection_changed) {
            select_object(session, document, mutation.selected_object_index);
        }
        if (mutation.clear_transient_state) {
            session.clear_transient_state();
        }
        result.preview_invalidated = mutation.changed || result.preview_invalidated;
        result.next_tool = AnnotationToolKind::Spline;
    }

    return result;
}

} // namespace mmltk::gui::tool_detail
