#pragma once

#include "gui/annotation/document/edit.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace mmltk::gui {

struct AnnotationSetObjectsCommand {
    std::vector<AnnotationObject> objects;
};

struct AnnotationInsertObjectCommand {
    AnnotationObject object;
    std::optional<std::size_t> index;
};

struct AnnotationReplaceObjectCommand {
    std::size_t index = 0;
    AnnotationObject object;
};

struct AnnotationSetObjectEnabledCommand {
    std::size_t index = 0;
    bool enabled = true;
};

struct AnnotationSetObjectCategoryCommand {
    std::size_t index = 0;
    std::size_t category_index = 0;
};

struct AnnotationRemapSkeletonCategoryCommand {
    std::size_t index = 0;
    std::size_t category_index = 0;
    AnnotationSkeletonShape shape;
};

struct AnnotationSetObjectColorRangesCommand {
    std::size_t index = 0;
    AnnotationColorRange sup{};
    AnnotationColorRange nosup{};
};

struct AnnotationRemoveObjectCommand {
    std::size_t index = 0;
};

struct AnnotationTranslateObjectCommand {
    std::size_t index = 0;
    int dx = 0;
    int dy = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationResizeObjectCommand {
    std::size_t index = 0;
    AnnotationBox box{};
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationSetObjectBoxCommand {
    std::size_t index = 0;
    AnnotationBox box{};
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationSetPointPositionCommand {
    std::size_t index = 0;
    AnnotationPoint point{};
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationSetHandlePositionCommand {
    AnnotationHandleId handle{};
    AnnotationPoint point{};
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationAppendSplineKnotCommand {
    std::size_t index = 0;
    AnnotationPoint point{};
};

struct AnnotationInsertSplineKnotCommand {
    std::size_t index = 0;
    std::size_t segment_index = 0;
    AnnotationPoint point{};
};

struct AnnotationRemoveSplineKnotCommand {
    std::size_t index = 0;
    std::size_t knot_index = 0;
};

struct AnnotationCloseSplineCommand {
    std::size_t index = 0;
};

struct AnnotationReopenSplineCommand {
    std::size_t index = 0;
};

struct AnnotationSetSplineKnotHandleModeCommand {
    std::size_t index = 0;
    std::size_t knot_index = 0;
    AnnotationSplineHandleMode mode = AnnotationSplineHandleMode::Corner;
};

struct AnnotationPlaceSkeletonNodeCommand {
    std::size_t index = 0;
    AnnotationPoint point{};
};

struct AnnotationPlaceSkeletonNodeAtCommand {
    std::size_t index = 0;
    std::size_t node_index = 0;
    AnnotationPoint point{};
};

struct AnnotationSetSkeletonNodeVisibilityCommand {
    std::size_t index = 0;
    std::size_t node_index = 0;
    bool visible = true;
};

struct AnnotationResetSkeletonNodeCommand {
    std::size_t index = 0;
    std::size_t node_index = 0;
};

struct AnnotationPaintMaskCommand {
    std::size_t index = 0;
    int capture_x = 0;
    int capture_y = 0;
    int radius = 1;
    bool erase = false;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationFillMaskCommand {
    std::size_t index = 0;
    int capture_x = 0;
    int capture_y = 0;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

struct AnnotationCleanupMaskCommand {
    std::size_t index = 0;
    AnnotationMaskCleanupOp op = AnnotationMaskCleanupOp::LargestComponent;
    int radius = 1;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
};

using AnnotationCommand = std::variant<
    AnnotationSetObjectsCommand,
    AnnotationInsertObjectCommand,
    AnnotationReplaceObjectCommand,
    AnnotationSetObjectEnabledCommand,
    AnnotationSetObjectCategoryCommand,
    AnnotationRemapSkeletonCategoryCommand,
    AnnotationSetObjectColorRangesCommand,
    AnnotationRemoveObjectCommand,
    AnnotationTranslateObjectCommand,
    AnnotationResizeObjectCommand,
    AnnotationSetObjectBoxCommand,
    AnnotationSetPointPositionCommand,
    AnnotationSetHandlePositionCommand,
    AnnotationAppendSplineKnotCommand,
    AnnotationInsertSplineKnotCommand,
    AnnotationRemoveSplineKnotCommand,
    AnnotationCloseSplineCommand,
    AnnotationReopenSplineCommand,
    AnnotationSetSplineKnotHandleModeCommand,
    AnnotationPlaceSkeletonNodeCommand,
    AnnotationPlaceSkeletonNodeAtCommand,
    AnnotationSetSkeletonNodeVisibilityCommand,
    AnnotationResetSkeletonNodeCommand,
    AnnotationPaintMaskCommand,
    AnnotationFillMaskCommand,
    AnnotationCleanupMaskCommand>;

struct AnnotationDocumentSnapshot {
    std::uint64_t generation = 0;
    std::vector<AnnotationObject> objects;
};

class AnnotationDocument {
public:
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::vector<AnnotationObject>& objects() const noexcept { return objects_; }
    [[nodiscard]] std::vector<AnnotationObject>& objects() noexcept { return objects_; }
    [[nodiscard]] bool empty() const noexcept { return objects_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }
    [[nodiscard]] bool can_undo() const noexcept { return !undo_stack_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_stack_.empty(); }
    [[nodiscard]] bool transaction_active() const noexcept { return transaction_active_; }
    [[nodiscard]] const AnnotationObject* object(std::size_t index) const noexcept;
    [[nodiscard]] AnnotationObject* object(std::size_t index) noexcept;
    [[nodiscard]] AnnotationDocumentSnapshot snapshot() const;

    void begin_transaction();
    bool commit_transaction();
    void cancel_transaction();
    bool undo();
    bool redo();
    void set_objects(std::vector<AnnotationObject> objects, bool clear_history = true);
    void clear_history();
    bool apply(const AnnotationCommand& command);
    void clear();

private:
    bool apply_without_history(const AnnotationCommand& command);

    std::vector<AnnotationObject> objects_;
    std::uint64_t generation_ = 0;
    std::vector<std::vector<AnnotationObject>> undo_stack_;
    std::vector<std::vector<AnnotationObject>> redo_stack_;
    bool transaction_active_ = false;
    bool transaction_changed_ = false;
    std::vector<AnnotationObject> transaction_before_;
};

} // namespace mmltk::gui
