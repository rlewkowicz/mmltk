#include "gui/annotation/document/document.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace mmltk::gui {

namespace {

bool color_range_equal(const AnnotationColorRange& lhs,
                       const AnnotationColorRange& rhs) {
    return lhs.center.hue_degrees == rhs.center.hue_degrees &&
           lhs.center.saturation == rhs.center.saturation &&
           lhs.center.value == rhs.center.value &&
           lhs.tolerance.hue_minus_pct == rhs.tolerance.hue_minus_pct &&
           lhs.tolerance.hue_plus_pct == rhs.tolerance.hue_plus_pct &&
           lhs.tolerance.saturation_minus_pct == rhs.tolerance.saturation_minus_pct &&
           lhs.tolerance.saturation_plus_pct == rhs.tolerance.saturation_plus_pct &&
           lhs.tolerance.value_minus_pct == rhs.tolerance.value_minus_pct &&
           lhs.tolerance.value_plus_pct == rhs.tolerance.value_plus_pct &&
           lhs.sampling == rhs.sampling;
}

bool skeleton_shape_equal(const AnnotationSkeletonShape& lhs,
                          const AnnotationSkeletonShape& rhs) {
    if (lhs.nodes.size() != rhs.nodes.size() ||
        lhs.edges.size() != rhs.edges.size()) {
        return false;
    }

    const auto nodes_equal = [] (const AnnotationSkeletonNode& lhs_node,
                                 const AnnotationSkeletonNode& rhs_node) {
        return lhs_node.key == rhs_node.key &&
               lhs_node.point.x == rhs_node.point.x &&
               lhs_node.point.y == rhs_node.point.y &&
               lhs_node.visible == rhs_node.visible;
    };
    if (!std::equal(lhs.nodes.begin(), lhs.nodes.end(), rhs.nodes.begin(), nodes_equal)) {
        return false;
    }

    const auto edges_equal = [] (const AnnotationSkeletonEdge& lhs_edge,
                                 const AnnotationSkeletonEdge& rhs_edge) {
        return lhs_edge.source_index == rhs_edge.source_index &&
               lhs_edge.target_index == rhs_edge.target_index;
    };
    return std::equal(lhs.edges.begin(), lhs.edges.end(), rhs.edges.begin(), edges_equal);
}

} // namespace

const AnnotationObject* AnnotationDocument::object(const std::size_t index) const noexcept {
    return index < objects_.size() ? &objects_[index] : nullptr;
}

AnnotationObject* AnnotationDocument::object(const std::size_t index) noexcept {
    return index < objects_.size() ? &objects_[index] : nullptr;
}

AnnotationDocumentSnapshot AnnotationDocument::snapshot() const {
    return AnnotationDocumentSnapshot{
        generation_,
        objects_,
    };
}

void AnnotationDocument::begin_transaction() {
    if (transaction_active_) {
        return;
    }
    transaction_active_ = true;
    transaction_changed_ = false;
    transaction_before_ = objects_;
}

bool AnnotationDocument::commit_transaction() {
    if (!transaction_active_) {
        return false;
    }
    const bool changed = transaction_changed_;
    if (changed) {
        undo_stack_.push_back(std::exchange(transaction_before_, {}));
        redo_stack_.clear();
    } else {
        transaction_before_.clear();
    }
    transaction_active_ = false;
    transaction_changed_ = false;
    return changed;
}

void AnnotationDocument::cancel_transaction() {
    if (!transaction_active_) {
        return;
    }
    if (transaction_changed_) {
        objects_ = std::exchange(transaction_before_, {});
        ++generation_;
    } else {
        transaction_before_.clear();
    }
    transaction_active_ = false;
    transaction_changed_ = false;
}

bool AnnotationDocument::undo() {
    if (transaction_active_ || undo_stack_.empty()) {
        return false;
    }
    redo_stack_.push_back(objects_);
    objects_ = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    ++generation_;
    return true;
}

bool AnnotationDocument::redo() {
    if (transaction_active_ || redo_stack_.empty()) {
        return false;
    }
    undo_stack_.push_back(objects_);
    objects_ = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    ++generation_;
    return true;
}

void AnnotationDocument::set_objects(std::vector<AnnotationObject> objects,
                                     const bool clear_history) {
    objects_ = std::move(objects);
    ++generation_;
    if (clear_history) {
        this->clear_history();
    }
}

void AnnotationDocument::clear_history() {
    undo_stack_.clear();
    redo_stack_.clear();
    transaction_active_ = false;
    transaction_changed_ = false;
    transaction_before_.clear();
}

bool AnnotationDocument::apply_without_history(const AnnotationCommand& command) {
    return std::visit(
        [this] (const auto& typed_command) {
            using T = std::decay_t<decltype(typed_command)>;
            if constexpr (std::is_same_v<T, AnnotationSetObjectsCommand>) {
                objects_ = typed_command.objects;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationInsertObjectCommand>) {
                const std::size_t insert_index =
                    std::min(typed_command.index.value_or(objects_.size()), objects_.size());
                objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(insert_index), typed_command.object);
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationReplaceObjectCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                objects_[typed_command.index] = typed_command.object;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationSetObjectEnabledCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                AnnotationObject& object = objects_[typed_command.index];
                if (object.enabled == typed_command.enabled) {
                    return false;
                }
                object.enabled = typed_command.enabled;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationSetObjectCategoryCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                AnnotationObject& object = objects_[typed_command.index];
                if (object.category_index == typed_command.category_index) {
                    return false;
                }
                object.category_index = typed_command.category_index;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationRemapSkeletonCategoryCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                AnnotationObject& object = objects_[typed_command.index];
                auto* skeleton = std::get_if<AnnotationSkeletonShape>(&object.shape);
                if (skeleton == nullptr) {
                    return false;
                }
                if (object.category_index == typed_command.category_index &&
                    skeleton_shape_equal(*skeleton, typed_command.shape)) {
                    return false;
                }
                object.category_index = typed_command.category_index;
                object.shape = typed_command.shape;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationSetObjectColorRangesCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                AnnotationObject& object = objects_[typed_command.index];
                if (color_range_equal(object.sup, typed_command.sup) &&
                    color_range_equal(object.nosup, typed_command.nosup)) {
                    return false;
                }
                object.sup = typed_command.sup;
                object.nosup = typed_command.nosup;
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationRemoveObjectCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(typed_command.index));
                return true;
            } else if constexpr (std::is_same_v<T, AnnotationTranslateObjectCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return translate_annotation_object(&objects_[typed_command.index],
                                                   typed_command.dx,
                                                   typed_command.dy,
                                                   typed_command.capture_width,
                                                   typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationResizeObjectCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return resize_annotation_object_to_box(&objects_[typed_command.index],
                                                       typed_command.box,
                                                       typed_command.capture_width,
                                                       typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationSetObjectBoxCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return set_annotation_object_box(&objects_[typed_command.index],
                                                 typed_command.box,
                                                 typed_command.capture_width,
                                                 typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationSetPointPositionCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return set_point_annotation_position(&objects_[typed_command.index],
                                                     typed_command.point,
                                                     typed_command.capture_width,
                                                     typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationSetHandlePositionCommand>) {
                if (typed_command.handle.object_index >= objects_.size()) {
                    return false;
                }
                return set_annotation_object_handle_position(&objects_[typed_command.handle.object_index],
                                                             typed_command.handle,
                                                             typed_command.point,
                                                             typed_command.capture_width,
                                                             typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationAppendSplineKnotCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return append_spline_knot(&objects_[typed_command.index], typed_command.point);
            } else if constexpr (std::is_same_v<T, AnnotationInsertSplineKnotCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return insert_spline_knot(&objects_[typed_command.index],
                                          typed_command.segment_index,
                                          typed_command.point);
            } else if constexpr (std::is_same_v<T, AnnotationRemoveSplineKnotCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return remove_spline_knot(&objects_[typed_command.index],
                                          typed_command.knot_index);
            } else if constexpr (std::is_same_v<T, AnnotationCloseSplineCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return close_spline_shape(&objects_[typed_command.index]);
            } else if constexpr (std::is_same_v<T, AnnotationReopenSplineCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return reopen_spline_shape(&objects_[typed_command.index]);
            } else if constexpr (std::is_same_v<T, AnnotationSetSplineKnotHandleModeCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return set_spline_knot_handle_mode(&objects_[typed_command.index],
                                                   typed_command.knot_index,
                                                   typed_command.mode);
            } else if constexpr (std::is_same_v<T, AnnotationPlaceSkeletonNodeCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return place_skeleton_node(&objects_[typed_command.index], typed_command.point);
            } else if constexpr (std::is_same_v<T, AnnotationPlaceSkeletonNodeAtCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return place_skeleton_node_at(&objects_[typed_command.index],
                                              typed_command.node_index,
                                              typed_command.point);
            } else if constexpr (std::is_same_v<T, AnnotationSetSkeletonNodeVisibilityCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return set_skeleton_node_visibility(&objects_[typed_command.index],
                                                    typed_command.node_index,
                                                    typed_command.visible);
            } else if constexpr (std::is_same_v<T, AnnotationResetSkeletonNodeCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return reset_skeleton_node(&objects_[typed_command.index],
                                           typed_command.node_index);
            } else if constexpr (std::is_same_v<T, AnnotationPaintMaskCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return paint_annotation_object_mask(&objects_[typed_command.index],
                                                    typed_command.capture_x,
                                                    typed_command.capture_y,
                                                    typed_command.radius,
                                                    typed_command.erase,
                                                    typed_command.capture_width,
                                                    typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationFillMaskCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return fill_annotation_object_mask(&objects_[typed_command.index],
                                                   typed_command.capture_x,
                                                   typed_command.capture_y,
                                                   typed_command.capture_width,
                                                   typed_command.capture_height);
            } else if constexpr (std::is_same_v<T, AnnotationCleanupMaskCommand>) {
                if (typed_command.index >= objects_.size()) {
                    return false;
                }
                return cleanup_annotation_object_mask(&objects_[typed_command.index],
                                                      typed_command.op,
                                                      typed_command.radius,
                                                      typed_command.capture_width,
                                                      typed_command.capture_height);
            } else {
                return false;
            }
        },
        command);
}

bool AnnotationDocument::apply(const AnnotationCommand& command) {
    const std::vector<AnnotationObject> before =
        transaction_active_ ? std::vector<AnnotationObject>{} : objects_;
    const bool changed = apply_without_history(command);
    if (!changed) {
        return false;
    }

    if (transaction_active_) {
        transaction_changed_ = true;
    } else {
        undo_stack_.push_back(before);
        redo_stack_.clear();
    }
    ++generation_;
    return true;
}

void AnnotationDocument::clear() {
    if (objects_.empty()) {
        return;
    }
    objects_.clear();
    ++generation_;
}

} // namespace mmltk::gui
