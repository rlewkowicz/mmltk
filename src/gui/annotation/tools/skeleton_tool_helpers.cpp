#include "gui/annotation/tools/skeleton_tool_helpers.h"

#include "gui/annotation/document/edit.h"
#include "gui/annotation/editor_history.h"
#include "gui/annotation/tools/tool_helpers.h"
#include "gui/annotation/workspace/model.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace mmltk::gui::tool_detail {

namespace {

[[nodiscard]] AnnotationSkeletonEditState make_skeleton_edit_state(
    const std::size_t object_index, const AnnotationSkeletonShape& shape,
    const std::optional<std::size_t> active_joint_index, const std::optional<std::size_t> last_placed_joint_index,
    const bool reseed_requested) noexcept {
    return AnnotationSkeletonEditState{
        object_index,
        clamp_active_index(shape.nodes.size(), active_joint_index),
        clamp_active_index(shape.nodes.size(), last_placed_joint_index),
        reseed_requested,
    };
}

[[nodiscard]] AnnotationToolMutation make_skeleton_tool_mutation(const bool changed) {
    return AnnotationToolMutation{
        changed,
        false,
        std::nullopt,
        false,
    };
}

std::optional<std::string> skeleton_joint_key_at(const AnnotationSkeletonShape& shape,
                                                 const std::optional<std::size_t> index) {
    if (!index.has_value() || *index >= shape.nodes.size()) {
        return std::nullopt;
    }
    return shape.nodes[*index].key;
}

std::string skeleton_joint_label(const AnnotationSkeletonNode& node, const std::size_t index) {
    std::string label = "#" + std::to_string(index + 1U);
    if (!node.key.empty()) {
        label += " ";
        label += node.key;
    }
    label += (node.visible ? " [visible]" : " [hidden]");
    return label;
}

void finish_grouped_skeleton_edit_if_complete(const AnnotationToolContext& context,
                                              const AnnotationSkeletonEditState& state) {
    if (!state.active_joint_index.has_value()) {
        (void)commit_grouped_tool_edit(context, AnnotationToolKind::Skeleton);
    }
}

template <typename ApplyFn, typename RefreshFn>
bool apply_and_refresh_skeleton_state(AnnotationDocument& document, const std::size_t selected_index,
                                      ApplyFn&& apply_fn, RefreshFn&& refresh_fn) {
    const bool changed = std::forward<ApplyFn>(apply_fn)();
    if (changed) {
        const AnnotationSkeletonShape* updated_skeleton =
            std::get_if<AnnotationSkeletonShape>(&document.object(selected_index)->shape);
        if (updated_skeleton != nullptr) {
            std::forward<RefreshFn>(refresh_fn)(*updated_skeleton);
        }
    }
    return changed;
}

template <typename ApplyFn>
AnnotationToolMutation place_selected_skeleton_node(const AnnotationToolContext& context,
                                                    const std::size_t selected_index,
                                                    const AnnotationSkeletonEditState& current_state,
                                                    ApplyFn&& apply_fn) {
    if (!begin_grouped_tool_edit(context, AnnotationToolKind::Skeleton, selected_index, selected_index)) {
        return {};
    }

    const bool changed = apply_and_refresh_skeleton_state(
        context.document, selected_index, std::forward<ApplyFn>(apply_fn), [&](const AnnotationSkeletonShape& updated) {
            set_active_skeleton_edit_state(context.session, selected_index, updated,
                                           next_skeleton_node_index(&updated, current_state.active_joint_index),
                                           current_state.active_joint_index, false);
            finish_grouped_skeleton_edit_if_complete(context, context.session.skeleton_edit_state());
        });
    return make_skeleton_tool_mutation(changed);
}

template <typename ApplyFn>
AnnotationToolMutation update_selected_active_skeleton_joint(const AnnotationToolContext& context,
                                                             const std::size_t selected_index,
                                                             const AnnotationSkeletonEditState& state,
                                                             const bool reseed_requested, ApplyFn&& apply_fn) {
    if (!state.active_joint_index.has_value()) {
        return {};
    }

    const std::size_t active_joint_index = *state.active_joint_index;
    return make_skeleton_tool_mutation(apply_and_refresh_skeleton_state(
        context.document, selected_index, [&] { return std::forward<ApplyFn>(apply_fn)(active_joint_index); },
        [&](const AnnotationSkeletonShape& updated) {
            set_active_skeleton_edit_state(context.session, selected_index, updated, state.active_joint_index,
                                           state.last_placed_joint_index, reseed_requested);
        }));
}

AnnotationToolMutation handle_skeleton_new_object_click(const AnnotationToolContext& context,
                                                        const AnnotationToolCanvasClickEvent& event,
                                                        const AnnotationPoint& point) {
    if (event.double_click) {
        return {};
    }

    const auto& restore_selection = context.session.selected_object_index();
    if (!begin_grouped_tool_edit(context, AnnotationToolKind::Skeleton, std::nullopt, restore_selection)) {
        return {};
    }

    const AnnotationCategory* category = event.default_category_index < context.categories.items.size()
                                             ? &context.categories.items[event.default_category_index]
                                             : nullptr;
    AnnotationObject object =
        make_skeleton_annotation_object(context.document.size(), event.default_category_index, category);
    if (!place_skeleton_node(&object, point) ||
        !context.document.apply(AnnotationInsertObjectCommand{std::move(object), std::nullopt})) {
        (void)cancel_grouped_tool_edit(context, AnnotationToolKind::Skeleton);
        return {};
    }

    const std::size_t inserted_index = context.document.size() - 1U;
    context.session.select_object(inserted_index);
    bind_grouped_tool_edit_object(context.session, inserted_index);
    finalize_created_skeleton_object(context.document, context.session, inserted_index);

    return make_skeleton_tool_mutation(true);
}

}  

void seed_created_skeleton_edit_state(AnnotationSession& session, const std::size_t object_index,
                                      const AnnotationObject& object) noexcept {
    const AnnotationSkeletonShape* skeleton = std::get_if<AnnotationSkeletonShape>(&object.shape);
    if (skeleton == nullptr) {
        return;
    }
    set_active_skeleton_edit_state(session, object_index, *skeleton, first_hidden_skeleton_node_index(skeleton),
                                   std::nullopt, false);
}

void finalize_created_skeleton_object(AnnotationDocument& document, AnnotationSession& session,
                                      const std::size_t object_index) {
    const AnnotationObject* object = document.object(object_index);
    if (object == nullptr) {
        return;
    }
    seed_created_skeleton_edit_state(session, object_index, *object);
    if (!session.skeleton_edit_state().active_joint_index.has_value()) {
        (void)commit_grouped_tool_edit(document, session, AnnotationToolKind::Skeleton);
    }
}

AnnotationSkeletonEditState current_or_seed_skeleton_edit_state(const AnnotationSession& session,
                                                                const std::size_t object_index,
                                                                const AnnotationSkeletonShape& shape) noexcept {
    if (session.skeleton_edit_state().object_index == object_index) {
        const AnnotationSkeletonEditState& current_state = session.skeleton_edit_state();
        return make_skeleton_edit_state(object_index, shape, current_state.active_joint_index,
                                        current_state.last_placed_joint_index, current_state.reseed_requested);
    }
    return make_skeleton_edit_state(object_index, shape, first_hidden_skeleton_node_index(&shape), std::nullopt, false);
}

AnnotationSidebarSelectedSkeletonViewModel make_selected_skeleton_sidebar_view_model(
    const AnnotationSession& session, const std::size_t object_index, const AnnotationSkeletonShape& shape) {
    const AnnotationSkeletonEditState edit_state = current_or_seed_skeleton_edit_state(session, object_index, shape);
    const AnnotationGroupedEditTransactionState grouped_edit = session.grouped_edit_transaction();
    const std::optional<std::size_t> active_joint_index = edit_state.active_joint_index;
    const std::optional<std::size_t> next_joint_index = next_skeleton_node_index(&shape, active_joint_index);
    AnnotationSidebarSelectedSkeletonViewModel model{
        visible_skeleton_node_count(shape),
        shape.nodes.size(),
        active_joint_index,
        skeleton_joint_key_at(shape, active_joint_index),
        next_joint_index,
        skeleton_joint_key_at(shape, next_joint_index),
        active_joint_index.has_value() && *active_joint_index < shape.nodes.size()
            ? shape.nodes[*active_joint_index].visible
            : false,
        edit_state.reseed_requested,
        next_joint_index.has_value(),
        active_joint_index.has_value() && *active_joint_index < shape.nodes.size() &&
            shape.nodes[*active_joint_index].visible,
        active_joint_index.has_value() && *active_joint_index < shape.nodes.size() &&
            !shape.nodes[*active_joint_index].visible,
        active_joint_index.has_value() && *active_joint_index < shape.nodes.size(),
        grouped_edit.kind == AnnotationEditTransactionKind::SkeletonConstruction &&
            grouped_edit.object_index == object_index,
        {},
    };
    model.joints.reserve(shape.nodes.size());
    for (std::size_t node_index = 0; node_index < shape.nodes.size(); ++node_index) {
        const AnnotationSkeletonNode& node = shape.nodes[node_index];
        model.joints.push_back(AnnotationSidebarSkeletonJointViewModel{
            node_index,
            skeleton_joint_label(node, node_index),
            node.visible,
            active_joint_index.has_value() && *active_joint_index == node_index,
        });
    }
    return model;
}

void set_active_skeleton_edit_state(AnnotationSession& session, const std::size_t object_index,
                                    const AnnotationSkeletonShape& shape,
                                    const std::optional<std::size_t> active_joint_index,
                                    const std::optional<std::size_t> last_placed_joint_index,
                                    const bool reseed_requested) noexcept {
    session.set_skeleton_edit_state(
        make_skeleton_edit_state(object_index, shape, active_joint_index, last_placed_joint_index, reseed_requested));
}

bool update_selected_skeleton_object_metadata(AnnotationDocument& document, AnnotationSession& session,
                                              const AnnotationCategories& categories, const bool enabled,
                                              const std::size_t category_index) {
    const std::size_t resolved_category_index =
        categories.items.empty() ? 0U : std::min(category_index, categories.items.size() - 1U);
    return apply_selected_object_edit(
        document, session, [&](const std::size_t selected_object_index, const AnnotationObject& object) {
            const bool enabled_changed = object.enabled != enabled;
            const bool category_changed = object.category_index != resolved_category_index;
            if (!enabled_changed && !category_changed) {
                return false;
            }

            return apply_selected_object_transaction(document, [&]() {
                bool changed = false;
                if (enabled_changed) {
                    changed = document.apply(AnnotationSetObjectEnabledCommand{
                                  selected_object_index,
                                  enabled,
                              }) ||
                              changed;
                }
                if (category_changed) {
                    if (std::holds_alternative<AnnotationSkeletonShape>(object.shape)) {
                        AnnotationObject updated_object = object;
                        updated_object.category_index = resolved_category_index;
                        const AnnotationCategory* target_category = resolved_category_index < categories.items.size()
                                                                        ? &categories.items[resolved_category_index]
                                                                        : nullptr;
                        (void)remap_skeleton_annotation_object_to_category(&updated_object, target_category);
                        AnnotationSkeletonShape remapped_shape =
                            std::get<AnnotationSkeletonShape>(std::move(updated_object.shape));
                        changed = document.apply(AnnotationRemapSkeletonCategoryCommand{
                                      selected_object_index,
                                      resolved_category_index,
                                      std::move(remapped_shape),
                                  }) ||
                                  changed;
                    } else {
                        changed = document.apply(AnnotationSetObjectCategoryCommand{
                                      selected_object_index,
                                      resolved_category_index,
                                  }) ||
                                  changed;
                    }
                }
                return changed;
            });
        });
}

AnnotationToolMutation handle_skeleton_canvas_click(const AnnotationToolContext& context,
                                                    const AnnotationToolCanvasClickEvent& event) {
    const AnnotationPoint point = capture_point_from_event(event);
    const auto& selected_index = context.session.selected_object_index();
    if (selected_index.has_value()) {
        const AnnotationObject* selected_object = context.document.object(*selected_index);
        if (selected_object != nullptr && std::holds_alternative<AnnotationSkeletonShape>(selected_object->shape)) {
            const AnnotationSkeletonShape* skeleton = std::get_if<AnnotationSkeletonShape>(&selected_object->shape);
            if (skeleton == nullptr) {
                return {};
            }
            const AnnotationSkeletonEditState current_state =
                current_or_seed_skeleton_edit_state(context.session, *selected_index, *skeleton);
            if (current_state.active_joint_index.has_value()) {
                return place_selected_skeleton_node(context, *selected_index, current_state, [&] {
                    return context.document.apply(AnnotationPlaceSkeletonNodeAtCommand{
                        *selected_index,
                        *current_state.active_joint_index,
                        point,
                    });
                });
            }

            return place_selected_skeleton_node(context, *selected_index, current_state, [&] {
                return context.document.apply(AnnotationPlaceSkeletonNodeCommand{
                    *selected_index,
                    point,
                });
            });
        }
    }

    return handle_skeleton_new_object_click(context, event, point);
}

AnnotationToolMutation handle_skeleton_action(const AnnotationToolContext& context,
                                              const AnnotationToolActionEvent& event) {
    const auto& selected_index = context.session.selected_object_index();
    if (!selected_index.has_value()) {
        return {};
    }
    const AnnotationObject* selected_object = context.document.object(*selected_index);
    const AnnotationSkeletonShape* skeleton =
        selected_object != nullptr ? std::get_if<AnnotationSkeletonShape>(&selected_object->shape) : nullptr;
    if (skeleton == nullptr) {
        return {};
    }

    AnnotationSkeletonEditState state =
        current_or_seed_skeleton_edit_state(context.session, *selected_index, *skeleton);

    switch (event.action) {
        case AnnotationToolActionKind::SkipJoint:
            state.active_joint_index = next_skeleton_node_index(skeleton, state.active_joint_index);
            context.session.set_skeleton_edit_state(state);
            finish_grouped_skeleton_edit_if_complete(context, state);
            return {};
        case AnnotationToolActionKind::HideJoint:
            return update_selected_active_skeleton_joint(
                context, *selected_index, state, false, [&](const std::size_t active_joint_index) {
                    return context.document.apply(AnnotationSetSkeletonNodeVisibilityCommand{
                        *selected_index,
                        active_joint_index,
                        false,
                    });
                });
        case AnnotationToolActionKind::ReactivateJoint:
            return update_selected_active_skeleton_joint(
                context, *selected_index, state, false, [&](const std::size_t active_joint_index) {
                    return context.document.apply(AnnotationSetSkeletonNodeVisibilityCommand{
                        *selected_index,
                        active_joint_index,
                        true,
                    });
                });
        case AnnotationToolActionKind::ReseedJoint:
            return update_selected_active_skeleton_joint(
                context, *selected_index, state, true, [&](const std::size_t active_joint_index) {
                    return context.document.apply(AnnotationResetSkeletonNodeCommand{
                        *selected_index,
                        active_joint_index,
                    });
                });
        case AnnotationToolActionKind::Cancel:
            if (cancel_grouped_tool_edit(context, AnnotationToolKind::Skeleton)) {
                return make_skeleton_tool_mutation(true);
            }
            context.session.clear_skeleton_edit_state();
            return {};
        case AnnotationToolActionKind::Confirm:
        case AnnotationToolActionKind::DeleteActiveElement:
        case AnnotationToolActionKind::CycleHandleMode:
        case AnnotationToolActionKind::ReopenSpline:
            break;
    }
    return {};
}

bool set_selected_skeleton_active_joint(AnnotationDocument& document, AnnotationSession& session,
                                        const std::optional<std::size_t> joint_index) {
    return apply_selected_object_edit(
        document, session,
        [&session, joint_index](const std::size_t selected_object_index, const AnnotationObject& object) {
            const AnnotationSkeletonShape* skeleton = std::get_if<AnnotationSkeletonShape>(&object.shape);
            if (skeleton == nullptr) {
                return false;
            }
            if (joint_index.has_value() && *joint_index >= skeleton->nodes.size()) {
                return false;
            }

            const AnnotationSkeletonEditState current_state =
                current_or_seed_skeleton_edit_state(session, selected_object_index, *skeleton);
            const AnnotationSkeletonEditState next_state{
                selected_object_index,
                joint_index,
                current_state.last_placed_joint_index,
                false,
            };
            if (current_state.object_index == next_state.object_index &&
                current_state.active_joint_index == next_state.active_joint_index &&
                current_state.last_placed_joint_index == next_state.last_placed_joint_index &&
                current_state.reseed_requested == next_state.reseed_requested) {
                return false;
            }
            session.set_skeleton_edit_state(next_state);
            return true;
        });
}

AnnotationSkeletonSidebarMutationResult apply_annotation_skeleton_sidebar_edit(
    AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame* frame,
    const AnnotationCategories& categories, const AnnotationSkeletonSidebarEditRequest& request) {
    if (!request.update_skeleton_active_joint && !request.skeleton_action.has_value()) {
        return {};
    }

    AnnotationSkeletonSidebarMutationResult result;
    AnnotationEditorTransactionScope transaction(document, true);

    if (request.update_skeleton_active_joint &&
        set_selected_skeleton_active_joint(document, session, request.skeleton_active_joint_index)) {
        result.next_tool = AnnotationToolKind::Skeleton;
    }

    if (request.skeleton_action.has_value() && frame != nullptr) {
        const AnnotationToolMutation mutation = handle_skeleton_action(
            AnnotationToolContext{
                document,
                session,
                *frame,
                categories,
                annotation_frame_capture_width(*frame),
                annotation_frame_capture_height(*frame),
            },
            AnnotationToolActionEvent{*request.skeleton_action});
        if (mutation.selection_changed) {
            select_object(session, document, mutation.selected_object_index);
        }
        if (mutation.clear_transient_state) {
            session.clear_transient_state();
        }
        result.preview_invalidated = mutation.changed || result.preview_invalidated;
        result.next_tool = AnnotationToolKind::Skeleton;
    }

    transaction.finish();
    return result;
}

}  
