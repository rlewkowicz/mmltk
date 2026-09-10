#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <deque>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <meta>
#include "src/controller/subsystems/annotation/detail/annotation_mask.h"

#include "src/controller/subsystems/annotation/detail/annotation_document.h"
#include "src/controller/subsystems/annotation/annotation_system.h"

namespace mmltk::controller::subsystems::annotation {
namespace {

using domain::AnnotationEditorFacts;
using domain::AnnotationSceneContent;

[[nodiscard]] bool annotation_diagnostics_enabled() noexcept {
    static const bool enabled = std::getenv("MMLTK_ANNOTATION_DIAGNOSTICS") != nullptr;
    return enabled;
}

inline constexpr std::size_t kHistoryCapacity = 32U;

enum class AnnotationPointerAction : std::uint8_t {
    Select,
    BoxDrag,
    PointPlace,
    Brush,
    Fill,
    SplineKnot,
    SkeletonJoint,
    HandleDrag,
    ColorSample
};
struct PointerFacts final {
    bool active = false;
    std::uint64_t interaction_id = 0U;
    AnnotationPointerAction action = AnnotationPointerAction::Select;
    domain::AnnotationPointerTarget target{};
    domain::AnnotationPoint origin{};
    domain::AnnotationPoint latest{};
    std::uint64_t sequence = 0U;
    bool brush = false;
    bool preview = false;
};
struct JournalEntry final {
    struct Facts final {};
    struct Object final {
        std::uint16_t index = 0U;
        bool before_present = true;
        bool after_present = true;
        domain::AnnotationObject before{};
        domain::AnnotationObject after{};
    };
    struct Category final {
        std::uint16_t index = 0U;
        bool before_present = true;
        bool after_present = true;
        domain::ArtifactClassName before{};
        domain::ArtifactClassName after{};
    };
    struct Objects final {
        std::vector<domain::AnnotationObject> before;
        std::vector<domain::AnnotationObject> after;
    };
    struct Frame final {
        std::uint32_t before_index = 0U;
        std::uint32_t after_index = 0U;
        bool before_ready = false;
        bool after_ready = false;
    };
    AnnotationEditorFacts before{};
    AnnotationEditorFacts after{};
    std::variant<Facts, Object, Category, Objects, Frame> mutation{};
};
struct DocumentState final {
    domain::AnnotationUiState ui{};
    PointerFacts pointer{};
    domain::AnnotationObject brush;
    domain::AnnotationObject preview;
    MaskScratch mask_scratch;
    std::deque<JournalEntry> undo{};
    std::deque<JournalEntry> redo{};
    std::string_view rejection = "The action requires a compatible selected object or handle";
};

[[nodiscard]] DocumentOutcome refused(DocumentState& state,
                                      const std::string_view reason = "The action requires a compatible selected object or handle") {
    state.rejection = reason;
    return DocumentOutcome::Rejected;
}

[[nodiscard]] DocumentOutcome capacity(const DocumentState&) { return DocumentOutcome::Capacity; }

// Open validates the imported scene; journal admission validates each committed mutation.
// Pointer previews never mutate this established document invariant.
[[nodiscard]] bool pointer_ready(const DocumentState& state) { return state.ui.scene.document.valid(); }
[[nodiscard]] bool current(const DocumentState& state) { return pointer_ready(state) && state.ui.valid(); }

[[nodiscard]] bool next_revision_available(const DocumentState& state) {
    return state.ui.document_revision != std::numeric_limits<std::uint64_t>::max() &&
           state.ui.scene_revision != std::numeric_limits<std::uint64_t>::max() &&
           state.ui.interaction_revision != std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] bool point_in_frame(const AnnotationSceneContent& scene, const domain::AnnotationPoint point) {
    return point.finite() &&
           (!scene.frame_ready || (point.x >= 0.0F && point.y >= 0.0F && point.x <= scene.frame_width && point.y <= scene.frame_height));
}

[[nodiscard]] domain::AnnotationBox box_between(const domain::AnnotationPoint first, const domain::AnnotationPoint second) {
    return {.first = {.x = std::min(first.x, second.x), .y = std::min(first.y, second.y)},
            .second = {.x = std::max(first.x, second.x), .y = std::max(first.y, second.y)}};
}

[[nodiscard]] bool object_valid_for(const domain::AnnotationObject& object, const std::size_t categories,
                                    const AnnotationSceneContent& scene) {
    if (!object.valid() || object.category >= categories || !point_in_frame(scene, object.point) ||
        !point_in_frame(scene, object.box.first) || !point_in_frame(scene, object.box.second)) {
        return false;
    }
    return std::ranges::all_of(object.mask_points, [&scene](const auto& point) { return point_in_frame(scene, point); }) &&
           std::ranges::all_of(object.spline_knots,
                               [&scene](const auto& knot) {
                                   return point_in_frame(scene, knot.point) && point_in_frame(scene, knot.in.point) &&
                                          point_in_frame(scene, knot.out.point);
                               }) &&
           std::ranges::all_of(object.skeleton_nodes, [&scene](const auto& node) { return point_in_frame(scene, node.point); });
}

struct ObjectAfterView final {
    const AnnotationSceneContent& scene;
    const JournalEntry* entry = nullptr;

    [[nodiscard]] std::size_t size() const {
        if (const auto* objects = entry ? std::get_if<JournalEntry::Objects>(&entry->mutation) : nullptr) return objects->after.size();
        const auto* mutation = entry ? std::get_if<JournalEntry::Object>(&entry->mutation) : nullptr;
        if (!mutation) return scene.objects.size();
        if (!mutation->before_present && mutation->after_present) return scene.objects.size() + 1U;
        if (mutation->before_present && !mutation->after_present) return scene.objects.size() - 1U;
        return scene.objects.size();
    }

    [[nodiscard]] const domain::AnnotationObject* at(const std::size_t index) const {
        if (const auto* objects = entry ? std::get_if<JournalEntry::Objects>(&entry->mutation) : nullptr)
            return index < objects->after.size() ? &objects->after[index] : nullptr;
        const auto* mutation = entry ? std::get_if<JournalEntry::Object>(&entry->mutation) : nullptr;
        if (!mutation) return index < scene.objects.size() ? &scene.objects[index] : nullptr;
        const std::size_t changed = mutation->index;
        if (mutation->before_present && mutation->after_present) {
            if (index >= scene.objects.size()) return nullptr;
            return index == changed ? &mutation->after : &scene.objects[index];
        }
        if (!mutation->before_present && mutation->after_present) {
            if (index >= size()) return nullptr;
            return index == changed ? &mutation->after : (index < changed ? &scene.objects[index] : &scene.objects[index - 1U]);
        }
        if (mutation->before_present && !mutation->after_present) {
            if (index >= size()) return nullptr;
            return index < changed ? &scene.objects[index] : &scene.objects[index + 1U];
        }
        return nullptr;
    }
};

[[nodiscard]] bool target_valid_for(const domain::AnnotationPointerTarget& target, const ObjectAfterView& objects) {
    if (!target.valid() || (target.object && *target.object >= objects.size()) || (target.element && !target.object) ||
        target.element.has_value() != target.role.has_value())
        return false;
    if (!target.element || !target.role) return true;
    const auto* object = objects.at(*target.object);
    if (!object) return false;
    switch (*target.role) {
        case domain::AnnotationHandleRole::BoxCorner:
            return (object->shape == domain::AnnotationShape::Box || object->shape == domain::AnnotationShape::Mask) &&
                   *target.element < 4U;
        case domain::AnnotationHandleRole::Point:
            return object->shape == domain::AnnotationShape::Point && *target.element == 0U;
        case domain::AnnotationHandleRole::SplineKnot:
        case domain::AnnotationHandleRole::SplineInHandle:
        case domain::AnnotationHandleRole::SplineOutHandle:
            return object->shape == domain::AnnotationShape::Spline && *target.element < object->spline_knots.size();
        case domain::AnnotationHandleRole::SkeletonNode:
            return object->shape == domain::AnnotationShape::Skeleton && *target.element < object->skeleton_nodes.size();
    }
    return false;
}

[[nodiscard]] bool facts_valid_for(const AnnotationSceneContent& scene, const AnnotationEditorFacts& facts,
                                   const JournalEntry* entry = nullptr) {
    if (!mmltk::frameworks::reflection::enum_contains(facts.tool)) { return false; }
    ObjectAfterView objects{.scene = scene, .entry = entry};
    if (facts.selected_object && *facts.selected_object >= objects.size()) return false;
    const auto* category = entry ? std::get_if<JournalEntry::Category>(&entry->mutation) : nullptr;
    const std::size_t category_count = scene.categories.size() +
                                       ((category && !category->before_present && category->after_present) ? 1U : 0U) -
                                       ((category && category->before_present && !category->after_present) ? 1U : 0U);
    if (facts.selected_category && *facts.selected_category >= category_count) return false;
    if (facts.selected_spline_segment) {
        if (!facts.selected_object) return false;
        const auto* object = objects.at(*facts.selected_object);
        if (!object || *facts.selected_spline_segment >= object->spline_knots.size()) return false;
    }
    if (facts.selected_skeleton_joint) {
        if (!facts.selected_object) return false;
        const auto* object = objects.at(*facts.selected_object);
        if (!object || *facts.selected_skeleton_joint >= object->skeleton_nodes.size()) return false;
    }
    for (std::size_t index = 0U; index != objects.size(); ++index) {
        const auto* object = objects.at(index);
        if (!object || !object_valid_for(*object, category_count, scene)) return false;
    }
    return true;
}

[[nodiscard]] bool entry_forward_valid(const DocumentState& state, const JournalEntry& entry) {
    if (entry.before != state.ui.editor || entry.mutation.valueless_by_exception()) return false;
    const auto& scene = state.ui.scene;
    const bool mutation_valid = std::visit(
        [&scene](const auto& mutation) {
            using Mutation = std::remove_cvref_t<decltype(mutation)>;
            if constexpr (std::same_as<Mutation, JournalEntry::Facts>) {
                return true;
            } else if constexpr (std::same_as<Mutation, JournalEntry::Object>) {
                if (mutation.before_present == mutation.after_present) {
                    if (!mutation.before_present || mutation.index >= scene.objects.size() ||
                        scene.objects[mutation.index] != mutation.before)
                        return false;
                } else if (mutation.before_present) {
                    if (mutation.index >= scene.objects.size() || scene.objects[mutation.index] != mutation.before) return false;
                } else if (mutation.index > scene.objects.size()) {
                    return false;
                }
                return !mutation.after_present || object_valid_for(mutation.after, scene.categories.size(), scene);
            } else if constexpr (std::same_as<Mutation, JournalEntry::Category>) {
                if (mutation.before_present == mutation.after_present) return false;
                if (mutation.before_present) {
                    return !scene.categories.empty() && mutation.index + 1U == scene.categories.size() &&
                           scene.categories[mutation.index] == mutation.before;
                }
                return mutation.index == scene.categories.size() && mutation.after.valid();
            } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
                return mutation.before == scene.objects;
            } else {
                return mutation.before_index == scene.frame_index && mutation.before_ready == scene.frame_ready;
            }
        },
        entry.mutation);
    return mutation_valid && facts_valid_for(scene, entry.after, &entry);
}

[[nodiscard]] JournalEntry inverse(JournalEntry entry) {
    std::swap(entry.before, entry.after);
    std::visit(
        [](auto& mutation) {
            using Mutation = std::remove_cvref_t<decltype(mutation)>;
            if constexpr (std::same_as<Mutation, JournalEntry::Object> || std::same_as<Mutation, JournalEntry::Category>) {
                std::swap(mutation.before_present, mutation.after_present);
                std::swap(mutation.before, mutation.after);
            } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
                std::swap(mutation.before, mutation.after);
            } else if constexpr (std::same_as<Mutation, JournalEntry::Frame>) {
                std::swap(mutation.before_index, mutation.after_index);
                std::swap(mutation.before_ready, mutation.after_ready);
            }
        },
        entry.mutation);
    return entry;
}

void apply_forward(DocumentState& state, const JournalEntry& entry) {
    auto& scene = state.ui.scene;
    std::visit(
        [&scene](const auto& mutation) {
            using Mutation = std::remove_cvref_t<decltype(mutation)>;
            if constexpr (std::same_as<Mutation, JournalEntry::Object>) {
                if (!mutation.before_present && mutation.after_present) {
                    scene.objects.insert(scene.objects.begin() + mutation.index, mutation.after);
                } else if (mutation.before_present && !mutation.after_present) {
                    scene.objects.erase(scene.objects.begin() + mutation.index);
                } else {
                    scene.objects[mutation.index] = mutation.after;
                }
            } else if constexpr (std::same_as<Mutation, JournalEntry::Category>) {
                if (!mutation.before_present && mutation.after_present)
                    scene.categories.push_back(mutation.after);
                else
                    scene.categories.pop_back();
                scene.palette = domain::annotation_class_palette(scene.categories.size());
            } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
                scene.objects = mutation.after;
            } else if constexpr (std::same_as<Mutation, JournalEntry::Frame>) {
                scene.frame_index = mutation.after_index;
                scene.frame_ready = mutation.after_ready;
            }
        },
        entry.mutation);
    state.ui.editor = entry.after;
}

void mark_edited(DocumentState& state) {
    state.pointer = {};
    ++state.ui.document_revision;
    ++state.ui.scene_revision;
    ++state.ui.interaction_revision;
    state.ui.scene.document.revision = state.ui.document_revision;
    state.ui.save_status = domain::AnnotationSaveStatus::Idle;
}

[[nodiscard]] bool entry_changes(const JournalEntry& entry) {
    if (entry.before != entry.after) return true;
    return std::visit(
        [](const auto& mutation) {
            using Mutation = std::remove_cvref_t<decltype(mutation)>;
            if constexpr (std::same_as<Mutation, JournalEntry::Facts>) {
                return false;
            } else if constexpr (std::same_as<Mutation, JournalEntry::Object> || std::same_as<Mutation, JournalEntry::Category>) {
                return mutation.before_present != mutation.after_present || mutation.before != mutation.after;
            } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
                return mutation.before != mutation.after;
            } else {
                return mutation.before_index != mutation.after_index || mutation.before_ready != mutation.after_ready;
            }
        },
        entry.mutation);
}

[[nodiscard]] DocumentOutcome commit(DocumentState& state, JournalEntry entry) {
    if (!current(state)) return refused(state);
    if (!entry_forward_valid(state, entry)) return refused(state);
    if (!entry_changes(entry)) return DocumentOutcome::Applied;
    if (!next_revision_available(state)) return refused(state);
    if (const auto* object = std::get_if<JournalEntry::Object>(&entry.mutation)) {
        std::size_t runs = object->after_present ? object->after.mask.runs.size() : 0U;
        if (runs > domain::kAnnotationMaskRunCapacity) return capacity(state);
        for (std::size_t index = 0U; index < state.ui.scene.objects.size(); ++index) {
            if (object->before_present && index == object->index) continue;
            const auto count = state.ui.scene.objects[index].mask.runs.size();
            if (count > domain::kAnnotationMaskRunCapacity - runs) return capacity(state);
            runs += count;
        }
    }
    if (state.undo.size() == kHistoryCapacity) state.undo.pop_front();
    state.undo.push_back(entry);
    apply_forward(state, entry);
    state.redo.clear();
    mark_edited(state);
    return DocumentOutcome::Applied;
}

[[nodiscard]] JournalEntry facts_entry(const AnnotationEditorFacts& before, const AnnotationEditorFacts& after) {
    return {.before = before, .after = after, .mutation = JournalEntry::Facts{}};
}

[[nodiscard]] DocumentOutcome change_editor_facts(DocumentState& state, const std::function_ref<void(AnnotationEditorFacts&)> mutate) {
    auto after = state.ui.editor;
    mutate(after);
    if (!facts_valid_for(state.ui.scene, after)) return refused(state);
    if (after == state.ui.editor) return DocumentOutcome::Applied;
    state.pointer = {};
    state.ui.editor = after;
    ++state.ui.interaction_revision;
    ++state.ui.scene_revision;
    return DocumentOutcome::Applied;
}

enum class JournalDirection : std::uint8_t {
    Undo,
    Redo,
};

[[nodiscard]] DocumentOutcome apply_journal(DocumentState& state, const JournalDirection direction) {
    auto& source = direction == JournalDirection::Undo ? state.undo : state.redo;
    auto& destination = direction == JournalDirection::Undo ? state.redo : state.undo;
    if (!current(state)) return refused(state);
    if (source.empty()) return DocumentOutcome::Applied;
    if (destination.size() == kHistoryCapacity) destination.pop_front();
    if (!next_revision_available(state)) return refused(state);

    const auto entry = source.back();
    auto applied = direction == JournalDirection::Undo ? inverse(entry) : entry;
    applied.before = state.ui.editor;
    if (!entry_forward_valid(state, applied)) return refused(state);
    destination.push_back(entry);
    apply_forward(state, applied);
    source.pop_back();
    mark_edited(state);
    return DocumentOutcome::Applied;
}

[[nodiscard]] JournalEntry journal_entry(
    const AnnotationEditorFacts& before, const AnnotationEditorFacts& after,
    std::variant<JournalEntry::Facts, JournalEntry::Object, JournalEntry::Category, JournalEntry::Objects, JournalEntry::Frame> mutation) {
    return {.before = before, .after = after, .mutation = std::move(mutation)};
}

[[nodiscard]] JournalEntry object_entry(const AnnotationEditorFacts& before, const AnnotationEditorFacts& after, const std::uint16_t index,
                                        const bool before_present, const domain::AnnotationObject& before_object, const bool after_present,
                                        const domain::AnnotationObject& after_object) {
    return journal_entry(before, after,
                         JournalEntry::Object{.index = index,
                                              .before_present = before_present,
                                              .after_present = after_present,
                                              .before = before_object,
                                              .after = after_object});
}

template <class Mutate>
[[nodiscard]] DocumentOutcome change_object(DocumentState& state, const std::uint16_t index, const AnnotationEditorFacts& before,
                                            AnnotationEditorFacts after, Mutate&& mutate) {
    const domain::AnnotationObject original = state.ui.scene.objects[index];
    domain::AnnotationObject changed = original;
    std::forward<Mutate>(mutate)(changed, after);
    // CLEANUP-IGNORE: Object mutation commit and category journal construction are distinct canonical variant
    // alternatives.
    return commit(state, object_entry(before, after, index, true, original, true, changed));
}

[[nodiscard]] JournalEntry category_entry(const AnnotationEditorFacts& before, const AnnotationEditorFacts& after,
                                          const std::uint16_t index, const bool before_present,
                                          const domain::ArtifactClassName& before_category, const bool after_present,
                                          const domain::ArtifactClassName& after_category) {
    return journal_entry(before, after,
                         JournalEntry::Category{.index = index,
                                                .before_present = before_present,
                                                .after_present = after_present,
                                                .before = before_category,
                                                .after = after_category});
}

[[nodiscard]] bool target_valid(const AnnotationSceneContent& scene, const AnnotationEditorFacts& editor,
                                const domain::AnnotationPointerTarget& target) {
    (void)editor;
    return target_valid_for(target, ObjectAfterView{.scene = scene});
}

[[nodiscard]] bool action_matches_tool(const AnnotationPointerAction action, const domain::AnnotationTool tool) {
    switch (action) {
        case AnnotationPointerAction::Select:
            return true;
        case AnnotationPointerAction::BoxDrag:
            return tool == domain::AnnotationTool::Box;
        case AnnotationPointerAction::PointPlace:
            return tool == domain::AnnotationTool::Point;
        case AnnotationPointerAction::Brush:
            return tool == domain::AnnotationTool::MaskPaint || tool == domain::AnnotationTool::MaskErase;
        case AnnotationPointerAction::Fill:
            return tool == domain::AnnotationTool::MaskFill;
        case AnnotationPointerAction::SplineKnot:
            return tool == domain::AnnotationTool::Spline;
        case AnnotationPointerAction::SkeletonJoint:
            return tool == domain::AnnotationTool::Skeleton;
        case AnnotationPointerAction::HandleDrag:
            return true;
        case AnnotationPointerAction::ColorSample:
            return tool == domain::AnnotationTool::ColorSample;
    }
    return false;
}

[[nodiscard]] AnnotationPointerAction pointer_action(const domain::AnnotationTool tool, const domain::AnnotationPointerTarget& target) {
    if (target.role == domain::AnnotationHandleRole::SplineInHandle || target.role == domain::AnnotationHandleRole::SplineOutHandle)
        return AnnotationPointerAction::HandleDrag;
    switch (tool) {
        case domain::AnnotationTool::Select:
            return AnnotationPointerAction::Select;
        case domain::AnnotationTool::Box:
            return AnnotationPointerAction::BoxDrag;
        case domain::AnnotationTool::MaskPaint:
        case domain::AnnotationTool::MaskErase:
            return AnnotationPointerAction::Brush;
        case domain::AnnotationTool::MaskFill:
            return AnnotationPointerAction::Fill;
        case domain::AnnotationTool::Spline:
            return AnnotationPointerAction::SplineKnot;
        case domain::AnnotationTool::Point:
            return AnnotationPointerAction::PointPlace;
        case domain::AnnotationTool::Skeleton:
            return AnnotationPointerAction::SkeletonJoint;
        case domain::AnnotationTool::ColorSample:
            return AnnotationPointerAction::ColorSample;
    }
    return AnnotationPointerAction::Select;
}

[[nodiscard]] bool selected_shape(const AnnotationSceneContent& scene, const std::optional<std::uint16_t> selected,
                                  const domain::AnnotationShape shape) {
    return selected && *selected < scene.objects.size() && scene.objects[*selected].shape == shape;
}

[[nodiscard]] bool tool_applicable(const AnnotationSceneContent& scene, domain::AnnotationTool tool, std::optional<std::uint16_t> target,
                                   bool pointer_target) noexcept {
    if (!scene.frame_ready || !mmltk::frameworks::reflection::enum_contains(tool)) return false;
    if (target && *target >= scene.objects.size()) return false;
    const auto is_shape = [&](domain::AnnotationShape shape) { return target && scene.objects[*target].shape == shape; };
    switch (tool) {
        case domain::AnnotationTool::Select:
            return true;
        case domain::AnnotationTool::MaskErase:
        case domain::AnnotationTool::MaskFill:
        case domain::AnnotationTool::ColorSample:
            return is_shape(domain::AnnotationShape::Mask);
        case domain::AnnotationTool::MaskPaint:
            return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Mask));
        case domain::AnnotationTool::Box:
            return !scene.categories.empty() &&
                   (!pointer_target || !target || is_shape(domain::AnnotationShape::Box) || is_shape(domain::AnnotationShape::Mask));
        case domain::AnnotationTool::Point:
            return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Point));
        case domain::AnnotationTool::Spline:
            return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Spline));
        case domain::AnnotationTool::Skeleton:
            return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Skeleton));
    }
    return false;
}

[[nodiscard]] bool continues_pointer(const PointerFacts& active, const mmltk::controller::AnnotationPointer& request,
                                     const AnnotationPointerAction action) {
    return active.active && active.interaction_id == request.interaction_id && request.sequence > active.sequence &&
           active.action == action && active.target == request.target;
}

void set_spline_handle(domain::AnnotationSplineKnot& knot, domain::AnnotationHandleRole role, domain::AnnotationPoint point,
                       const AnnotationSceneContent& scene) {
    auto& selected = role == domain::AnnotationHandleRole::SplineInHandle ? knot.in : knot.out;
    auto& opposite = role == domain::AnnotationHandleRole::SplineInHandle ? knot.out : knot.in;
    selected = {.point = point, .enabled = true};
    if (knot.mode == domain::AnnotationSplineHandleMode::Corner) return;
    const float dx = knot.point.x - point.x, dy = knot.point.y - point.y;
    const float length = std::hypot(dx, dy);
    if (length == 0) return;
    float scale = knot.mode == domain::AnnotationSplineHandleMode::Mirrored
                      ? 1.0F
                      : std::hypot(opposite.point.x - knot.point.x, opposite.point.y - knot.point.y) / length;
    if (dx > 0) scale = std::min(scale, (scene.frame_width - knot.point.x) / dx);
    if (dx < 0) scale = std::min(scale, -knot.point.x / dx);
    if (dy > 0) scale = std::min(scale, (scene.frame_height - knot.point.y) / dy);
    if (dy < 0) scale = std::min(scale, -knot.point.y / dy);
    opposite = {.point = {knot.point.x + dx * scale, knot.point.y + dy * scale}, .enabled = true};
}
void drag_object(domain::AnnotationObject& object, const domain::AnnotationPointerTarget& target, const domain::AnnotationPoint origin,
                 const domain::AnnotationPoint point, const AnnotationSceneContent& scene, MaskScratch& scratch) {
    const auto original_box = object.box;
    if (target.role == domain::AnnotationHandleRole::BoxCorner) {
        const auto corner = *target.element;
        const domain::AnnotationPoint anchor{corner == 0 || corner == 3 ? object.box.second.x : object.box.first.x,
                                             corner < 2 ? object.box.second.y : object.box.first.y};
        const domain::AnnotationPoint corner_point{corner == 0 || corner == 3 ? object.box.first.x : object.box.second.x,
                                                   corner < 2 ? object.box.first.y : object.box.second.y};
        object.box = box_between(anchor, {std::clamp(corner_point.x + point.x - origin.x, 0.0F, static_cast<float>(scene.frame_width)),
                                          std::clamp(corner_point.y + point.y - origin.y, 0.0F, static_cast<float>(scene.frame_height))});
    } else if (target.role == domain::AnnotationHandleRole::Point)
        object.point = point;
    else if (target.role == domain::AnnotationHandleRole::SplineKnot) {
        auto& knot = object.spline_knots[*target.element];
        const float dx = point.x - knot.point.x, dy = point.y - knot.point.y;
        for (auto* handle : {&knot.in, &knot.out}) {
            handle->point.x = std::clamp(handle->point.x + dx, 0.0F, static_cast<float>(scene.frame_width));
            handle->point.y = std::clamp(handle->point.y + dy, 0.0F, static_cast<float>(scene.frame_height));
        }
        knot.point = point;
    } else if (target.role == domain::AnnotationHandleRole::SplineInHandle || target.role == domain::AnnotationHandleRole::SplineOutHandle)
        set_spline_handle(object.spline_knots[*target.element], *target.role, point, scene);
    else if (target.role == domain::AnnotationHandleRole::SkeletonNode)
        object.skeleton_nodes[*target.element].point = point;
    else if (object.shape == domain::AnnotationShape::Box || object.shape == domain::AnnotationShape::Mask) {
        const float dx = std::clamp(point.x - origin.x, -object.box.first.x, scene.frame_width - object.box.second.x);
        const float dy = std::clamp(point.y - origin.y, -object.box.first.y, scene.frame_height - object.box.second.y);
        object.box.first.x += dx;
        object.box.second.x += dx;
        object.box.first.y += dy;
        object.box.second.y += dy;
    }
    if (object.shape == domain::AnnotationShape::Mask) transform_mask(object, original_box, object.box, scratch);
}
void update_drag_preview(DocumentState& state, const domain::AnnotationPoint point) {
    auto& pointer = state.pointer;
    if (pointer.action == AnnotationPointerAction::BoxDrag && !pointer.target.object && !state.ui.scene.categories.empty() &&
        state.ui.scene.objects.size() < domain::kAnnotationObjectCapacity) {
        auto& object = state.preview;
        const domain::AnnotationObject empty;
        object = empty;
        object.name = domain::AnnotationText::From("box");
        object.shape = domain::AnnotationShape::Box;
        object.category = state.ui.editor.selected_category.value_or(0U);
        object.box = box_between(pointer.origin, point);
        pointer.preview = true;
    } else if ((pointer.action == AnnotationPointerAction::Select || pointer.action == AnnotationPointerAction::HandleDrag) &&
               pointer.target.object) {
        auto& object = state.preview;
        object = state.ui.scene.objects[*pointer.target.object];
        drag_object(object, pointer.target, pointer.origin, point, state.ui.scene, state.mask_scratch);
        pointer.preview = true;
    }
}
}  // namespace

[[nodiscard]] DocumentOutcome reduce_pointer(DocumentState& state, const mmltk::controller::AnnotationPointer& request,
                                             const std::uint16_t brush_radius) {
    const auto tool = state.ui.editor.tool;
    const auto action = pointer_action(tool, request.target);
    const auto refuse_pointer = [&](const std::string_view reason = "Pointer coordinates or target are invalid for this tool") {
        if ((request.phase == domain::AnnotationPointerPhase::End || request.phase == domain::AnnotationPointerPhase::Cancel) &&
            state.pointer.active && state.pointer.interaction_id == request.interaction_id) {
            state.pointer = {};
        }
        return refused(state, reason);
    };
    if (!pointer_ready(state)) return refused(state, "Open an image before editing");
    if (!request.valid() || !mmltk::frameworks::reflection::enum_contains(request.phase) ||
        !mmltk::frameworks::reflection::enum_contains(tool) || request.interaction_id == 0U || request.sequence == 0U ||
        !point_in_frame(state.ui.scene, request.point) || !target_valid(state.ui.scene, state.ui.editor, request.target) ||
        (!action_matches_tool(action, tool) || !tool_applicable(state.ui.scene, tool, request.target.object, true))) {
        return refuse_pointer();
    }

    const auto before = state.ui.editor;
    auto after = before;
    if (request.phase == domain::AnnotationPointerPhase::Begin) {
        if (request.sequence != 1U || state.pointer.active) {
            return refused(state, "A new gesture requires sequence one and no active gesture");
        }
        state.pointer = {.active = true,
                         .interaction_id = request.interaction_id,
                         .action = action,
                         .target = request.target,
                         .origin = request.point,
                         .latest = request.point,
                         .sequence = request.sequence};
        if (action == AnnotationPointerAction::Brush) {
            if (!state.ui.scene.frame_ready || state.ui.scene.categories.empty()) {
                state.pointer = {};
                return refused(state);
            }
            if (!request.target.object && state.ui.scene.objects.size() >= domain::kAnnotationObjectCapacity) {
                state.pointer = {};
                return capacity(state);
            }
            auto& brush = state.brush;
            if (request.target.object)
                brush = state.ui.scene.objects[*request.target.object];
            else {
                if (tool == domain::AnnotationTool::MaskErase) {
                    state.pointer = {};
                    return refused(state);
                }
                const domain::AnnotationObject empty;
                brush = empty;
                brush.name = domain::AnnotationText::From("mask");
                brush.shape = domain::AnnotationShape::Mask;
                brush.category = after.selected_category.value_or(0U);
            }
            if (brush.shape != domain::AnnotationShape::Mask) {
                state.pointer = {};
                return refused(state);
            }
            stroke_mask(brush, request.point, request.point, brush_radius, state.ui.scene.frame_width, state.ui.scene.frame_height,
                        tool == domain::AnnotationTool::MaskErase, state.mask_scratch);
            state.pointer.brush = true;
        }
        update_drag_preview(state, request.point);
        return DocumentOutcome::Applied;
    }
    if (request.phase == domain::AnnotationPointerPhase::Update) {
        if (!continues_pointer(state.pointer, request, action)) {
            return refuse_pointer("Gesture identity, target or sequence does not match the active gesture");
        }
        if (state.pointer.brush)
            stroke_mask(state.brush, state.pointer.latest, request.point, brush_radius, state.ui.scene.frame_width,
                        state.ui.scene.frame_height, tool == domain::AnnotationTool::MaskErase, state.mask_scratch);
        update_drag_preview(state, request.point);
        state.pointer.latest = request.point;
        state.pointer.sequence = request.sequence;
        return DocumentOutcome::Applied;
    }
    if (request.phase == domain::AnnotationPointerPhase::Cancel) {
        if (!continues_pointer(state.pointer, request, action)) {
            return refuse_pointer("Gesture identity, target or sequence does not match the active gesture");
        }
        state.pointer = {};
        return DocumentOutcome::Applied;
    }
    if (!continues_pointer(state.pointer, request, action)) {
        return refuse_pointer("Gesture identity, target or sequence does not match the active gesture");
    }

    after.tool = tool;
    const auto origin = state.pointer.origin;
    const auto target = state.pointer.target;
    auto* brush = state.pointer.brush ? &state.brush : nullptr;
    if (brush)
        stroke_mask(*brush, state.pointer.latest, request.point, brush_radius, state.ui.scene.frame_width, state.ui.scene.frame_height,
                    tool == domain::AnnotationTool::MaskErase, state.mask_scratch);
    state.pointer = {};
    const auto& scene = state.ui.scene;
    switch (action) {
        case AnnotationPointerAction::Select:
            if (target.object && request.point != origin) {
                const auto& existing = scene.objects[*target.object];
                auto object = existing;
                drag_object(object, target, origin, request.point, scene, state.mask_scratch);
                after.selected_object = target.object;
                return commit(state, object_entry(before, after, *target.object, true, existing, true, object));
            }
            after.selected_object = target.object;
            after.selected_spline_segment.reset();
            after.selected_skeleton_joint.reset();
            if (after == before) return DocumentOutcome::Applied;
            return change_editor_facts(state, [&after](auto& facts) { facts = after; });
        case AnnotationPointerAction::BoxDrag: {
            if (origin == request.point) return DocumentOutcome::Applied;
            after.selected_spline_segment.reset();
            after.selected_skeleton_joint.reset();
            if (!target.object) {
                if (scene.categories.empty() || scene.objects.size() >= domain::kAnnotationObjectCapacity) return capacity(state);
                domain::AnnotationObject object{};
                object.name = domain::AnnotationText::From("box");
                object.shape = domain::AnnotationShape::Box;
                object.box = box_between(origin, request.point);
                object.category = after.selected_category.value_or(0U);
                after.selected_object = static_cast<std::uint16_t>(scene.objects.size());
                return commit(state,
                              object_entry(before, after, static_cast<std::uint16_t>(scene.objects.size()), false, {}, true, object));
            }
            const auto& existing = scene.objects[*target.object];
            if (existing.shape != domain::AnnotationShape::Box && existing.shape != domain::AnnotationShape::Mask) return refuse_pointer();
            auto object = existing;
            object.box = box_between(origin, request.point);
            if (object.shape == domain::AnnotationShape::Mask) transform_mask(object, existing.box, object.box, state.mask_scratch);
            after.selected_object = *target.object;
            return commit(state, object_entry(before, after, *target.object, true, existing, true, object));
        }
        case AnnotationPointerAction::PointPlace:
        case AnnotationPointerAction::Brush:
        case AnnotationPointerAction::Fill:
        case AnnotationPointerAction::SplineKnot:
        case AnnotationPointerAction::SkeletonJoint:
        case AnnotationPointerAction::HandleDrag:
        case AnnotationPointerAction::ColorSample: {
            if (!target.object && (scene.categories.empty() || scene.objects.size() >= domain::kAnnotationObjectCapacity))
                return capacity(state);
            const auto index = target.object.value_or(static_cast<std::uint16_t>(scene.objects.size()));
            const domain::AnnotationObject existing = target.object ? scene.objects[index] : domain::AnnotationObject{};
            auto object = existing;
            if (!target.object) {
                object.category = after.selected_category.value_or(0U);
                object.name = domain::AnnotationText::From("annotation");
                switch (action) {
                    case AnnotationPointerAction::PointPlace:
                        object.shape = domain::AnnotationShape::Point;
                        break;
                    case AnnotationPointerAction::Brush:
                    case AnnotationPointerAction::Fill:
                        object.shape = domain::AnnotationShape::Mask;
                        break;
                    case AnnotationPointerAction::SplineKnot:
                        object.shape = domain::AnnotationShape::Spline;
                        break;
                    case AnnotationPointerAction::SkeletonJoint:
                        object.shape = domain::AnnotationShape::Skeleton;
                        break;
                    default:
                        return refuse_pointer();
                }
            }
            switch (action) {
                case AnnotationPointerAction::PointPlace:
                    if (object.shape != domain::AnnotationShape::Point) return refuse_pointer();
                    object.point = request.point;
                    break;
                case AnnotationPointerAction::Brush:
                    if (object.shape != domain::AnnotationShape::Mask) return refuse_pointer();
                    if (!brush) return refuse_pointer();
                    object = *brush;
                    break;
                case AnnotationPointerAction::Fill:
                    if (object.shape != domain::AnnotationShape::Mask) return refuse_pointer();
                    fill_mask(object, request.point, scene.frame_width, scene.frame_height);
                    break;
                case AnnotationPointerAction::SplineKnot:
                    if (object.shape != domain::AnnotationShape::Spline) return refuse_pointer();
                    if (object.spline_knots.size() >= domain::kAnnotationGeometryCapacity) return capacity(state);
                    object.spline_knots.push_back({.point = request.point});
                    after.selected_spline_segment = static_cast<std::uint16_t>(object.spline_knots.size() - 1U);
                    break;
                case AnnotationPointerAction::SkeletonJoint:
                    if (object.shape != domain::AnnotationShape::Skeleton) return refuse_pointer();
                    if (object.skeleton_nodes.size() >= domain::kAnnotationGeometryCapacity) return capacity(state);
                    if (!object.skeleton_nodes.empty())
                        object.skeleton_edges.push_back({static_cast<std::uint16_t>(object.skeleton_nodes.size() - 1),
                                                         static_cast<std::uint16_t>(object.skeleton_nodes.size())});
                    object.skeleton_nodes.push_back({.key = domain::AnnotationText::From("joint"), .point = request.point});
                    after.selected_skeleton_joint = static_cast<std::uint16_t>(object.skeleton_nodes.size() - 1U);
                    break;
                case AnnotationPointerAction::HandleDrag:
                    if (object.shape != domain::AnnotationShape::Spline || !target.element || !target.role) return refuse_pointer();
                    set_spline_handle(object.spline_knots[*target.element], *target.role, request.point, scene);
                    break;
                case AnnotationPointerAction::ColorSample:
                    return DocumentOutcome::Applied;  // The native clean-image owner applies the sampled color.
                    break;
                default:
                    return refuse_pointer();
            }
            after.selected_object = index;
            if (object.shape != domain::AnnotationShape::Spline) after.selected_spline_segment.reset();
            if (object.shape != domain::AnnotationShape::Skeleton) after.selected_skeleton_joint.reset();
            return commit(state, object_entry(before, after, index, target.object.has_value(), existing, true, object));
        }
    }
    return refuse_pointer();
}

[[nodiscard]] DocumentOutcome reduce_tool(DocumentState& state, const domain::AnnotationTool tool) {
    if (!current(state) || !tool_applicable(state.ui.scene, tool, state.ui.editor.selected_object, false))
        return refused(state, "The selected tool is unavailable for this image and object");
    return change_editor_facts(state, [tool](AnnotationEditorFacts& editor) { editor.tool = tool; });
}

[[nodiscard]] DocumentOutcome reduce_setup(DocumentState& state, const domain::AnnotationSetupAction action) {
    if (!current(state) || !mmltk::frameworks::reflection::enum_contains(action)) return refused(state);
    return refused(state, "This imported image has no live or timeline frame provider");
}

[[nodiscard]] DocumentOutcome reduce_hold(DocumentState& state, const bool enabled) {
    // CLEANUP-IGNORE: Hold and tool requests mutate distinct editor facts through the shared change_editor_facts owner.
    if (!current(state)) return refused(state);
    return change_editor_facts(state, [enabled](AnnotationEditorFacts& editor) { editor.hold_save = enabled; });
}

[[nodiscard]] DocumentOutcome reduce_object(DocumentState& state, const std::uint16_t object) {
    if (!current(state) || object >= state.ui.scene.objects.size()) return refused(state);
    return change_editor_facts(state, [object](AnnotationEditorFacts& editor) {
        editor.selected_object = object;
        editor.selected_spline_segment.reset();
        editor.selected_skeleton_joint.reset();
    });
}

[[nodiscard]] DocumentOutcome reduce_category(DocumentState& state, const domain::AnnotationText& category) {
    if (!current(state) || !category.valid()) return refused(state);
    const auto& scene = state.ui.scene;
    if (scene.categories.size() >= domain::kAnnotationCategoryCapacity) return capacity(state);
    auto after = state.ui.editor;
    after.selected_category = static_cast<std::uint16_t>(scene.categories.size());
    return commit(state, category_entry(state.ui.editor, after, static_cast<std::uint16_t>(scene.categories.size()), false, {}, true,
                                        {.value = std::string{category.view()}}));
}

[[nodiscard]] DocumentOutcome reduce_object_facts(DocumentState& state, const std::uint16_t category, const bool enabled) {
    if (!current(state) || !state.ui.editor.selected_object || category >= state.ui.scene.categories.size()) return refused(state);
    const auto index = *state.ui.editor.selected_object;
    return change_object(state, index, state.ui.editor, state.ui.editor,
                         [category, enabled](domain::AnnotationObject& object, AnnotationEditorFacts&) {
                             object.category = category;
                             object.enabled = enabled;  // CLEANUP-IGNORE: CPD crosses distinct object-facts and
                                                        // spline-selection reducer transitions.
                         });
}

[[nodiscard]] DocumentOutcome reduce_spline(DocumentState& state, const std::uint16_t segment) {
    const auto& scene = state.ui.scene;
    if (!current(state) || !selected_shape(scene, state.ui.editor.selected_object, domain::AnnotationShape::Spline) ||
        segment >= scene.objects[*state.ui.editor.selected_object].spline_knots.size())
        return refused(state);
    return change_editor_facts(state,
                               // CLEANUP-IGNORE: Spline and skeleton selection are distinct domain transitions sharing the canonical
                               // editor-fact commit path.
                               [segment](AnnotationEditorFacts& editor) { editor.selected_spline_segment = segment; });
}

[[nodiscard]] DocumentOutcome reduce_handle(DocumentState& state, const domain::AnnotationHandleRole handle,
                                            const domain::AnnotationSplineHandleMode mode, const domain::AnnotationPoint point) {
    const auto& scene = state.ui.scene;
    if (!current(state) || !selected_shape(scene, state.ui.editor.selected_object, domain::AnnotationShape::Spline) ||
        !state.ui.editor.selected_spline_segment || !point_in_frame(scene, point) ||
        (handle != domain::AnnotationHandleRole::SplineInHandle && handle != domain::AnnotationHandleRole::SplineOutHandle) ||
        !mmltk::frameworks::reflection::enum_contains(mode))
        return refused(state);
    const auto index = *state.ui.editor.selected_object;
    return change_object(state, index, state.ui.editor, state.ui.editor,
                         [handle, mode, point, &state, &scene](domain::AnnotationObject& object, AnnotationEditorFacts&) {
                             auto& knot = object.spline_knots[*state.ui.editor.selected_spline_segment];
                             knot.mode = mode;
                             set_spline_handle(knot, handle, point, scene);
                         });
}

[[nodiscard]] DocumentOutcome reduce_skeleton(DocumentState& state, const std::uint16_t joint) {
    const auto& scene = state.ui.scene;
    if (!current(state) || !selected_shape(scene, state.ui.editor.selected_object, domain::AnnotationShape::Skeleton) ||
        joint >= scene.objects[*state.ui.editor.selected_object].skeleton_nodes.size())
        return refused(state);
    return change_editor_facts(state, [joint](AnnotationEditorFacts& editor) { editor.selected_skeleton_joint = joint; });
}

[[nodiscard]] DocumentOutcome reduce_mask_cleanup(DocumentState& state, const domain::AnnotationMaskCleanup operation,
                                                  const std::uint16_t cleanup_radius) {
    const auto& scene = state.ui.scene;
    if (!current(state) || !selected_shape(scene, state.ui.editor.selected_object, domain::AnnotationShape::Mask) ||
        cleanup_radius < domain::kMinAnnotationMaskCleanupRadius || cleanup_radius > domain::kMaxAnnotationMaskCleanupRadius ||
        !mmltk::frameworks::reflection::enum_contains(operation))
        return refused(state);
    const auto index = *state.ui.editor.selected_object;
    return change_object(state, index, state.ui.editor, state.ui.editor,
                         [operation, cleanup_radius, &scene](domain::AnnotationObject& object, AnnotationEditorFacts&) {
                             cleanup_mask(object, operation, cleanup_radius, scene.frame_width, scene.frame_height);
                         });
}

[[nodiscard]] DocumentOutcome reduce_mask_colors(DocumentState& state, const domain::AnnotationColorRange& sup,
                                                 const domain::AnnotationColorRange& nosup) {
    if (!current(state) || !selected_shape(state.ui.scene, state.ui.editor.selected_object, domain::AnnotationShape::Mask) ||
        // CLEANUP-IGNORE: Mask-color validation and object-category validation guard different domain invariants.
        !sup.valid() || !nosup.valid())
        return refused(state);
    const auto index = *state.ui.editor.selected_object;
    return change_object(state, index, state.ui.editor, state.ui.editor,
                         [&sup, &nosup](domain::AnnotationObject& object, AnnotationEditorFacts&) {
                             object.sup = sup;
                             object.nosup = nosup;
                         });
}

[[nodiscard]] DocumentOutcome reduce_scene(DocumentState& state) {
    if (!current(state) || !next_revision_available(state)) { return refused(state); }
    if (state.ui.scene.objects.empty()) return DocumentOutcome::Applied;
    auto after = state.ui.editor;
    after.selected_object.reset();
    after.selected_spline_segment.reset();
    after.selected_skeleton_joint.reset();
    return commit(
        state,
        {.before = state.ui.editor, .after = after, .mutation = JournalEntry::Objects{.before = state.ui.scene.objects, .after = {}}});
}

[[nodiscard]] DocumentOutcome reduce_open(DocumentState& state, domain::AnnotationSceneContent content) {
    if (state.ui.document_revision == std::numeric_limits<std::uint64_t>::max() || !content.valid()) return DocumentOutcome::Rejected;
    DocumentState replacement{};
    const std::uint64_t revision = state.ui.document_revision + 1U;
    replacement.ui.scene = std::move(content);
    replacement.ui.document_revision = revision;
    replacement.ui.scene_revision = revision;
    replacement.ui.interaction_revision = revision;
    replacement.ui.scene.document.revision = revision;
    if (!replacement.ui.scene.categories.empty()) replacement.ui.editor.selected_category = 0U;
    state = std::move(replacement);
    return DocumentOutcome::Applied;
}

[[nodiscard]] DocumentOutcome reduce_undo(DocumentState& state);
[[nodiscard]] DocumentOutcome reduce_redo(DocumentState& state);

[[nodiscard]] DocumentOutcome reduce_sidebar(DocumentState& state, const domain::AnnotationSidebarCommand command) {
    if (!current(state) || !mmltk::frameworks::reflection::enum_contains(command)) return refused(state);
    if (command == domain::AnnotationSidebarCommand::Undo) return reduce_undo(state);
    if (command == domain::AnnotationSidebarCommand::Redo) return reduce_redo(state);

    const auto& scene = state.ui.scene;
    const auto before = state.ui.editor;
    auto after = before;
    switch (command) {
        case domain::AnnotationSidebarCommand::Assist:
            if (!after.assist_available) return refused(state);
            after.assist_running = true;
            return commit(state, facts_entry(before, after));
        case domain::AnnotationSidebarCommand::Delete: {
            if (!before.selected_object) return refused(state);
            const auto index = *before.selected_object;
            const auto object = scene.objects[index];
            after.selected_object.reset();
            after.selected_spline_segment.reset();
            after.selected_skeleton_joint.reset();
            return commit(state, object_entry(before, after, index, true, object, false, {}));
        }
        case domain::AnnotationSidebarCommand::Duplicate: {
            if (!before.selected_object) return refused(state);
            if (scene.objects.size() >= domain::kAnnotationObjectCapacity) return capacity(state);
            const auto object = scene.objects[*before.selected_object];
            after.selected_object = static_cast<std::uint16_t>(scene.objects.size());
            after.selected_spline_segment.reset();
            after.selected_skeleton_joint.reset();
            return commit(state, object_entry(before, after, static_cast<std::uint16_t>(scene.objects.size()), false, {}, true, object));
        }
        case domain::AnnotationSidebarCommand::RedrawBox:
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Box) || before.tool == domain::AnnotationTool::Box)
                return refused(state);
            after.tool = domain::AnnotationTool::Box;
            return commit(state, facts_entry(before, after));
        case domain::AnnotationSidebarCommand::SplineInsertKnot: {
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Spline)) return refused(state);
            const auto index = *before.selected_object;
            const auto object = scene.objects[index];
            if (object.spline_knots.size() >= domain::kAnnotationGeometryCapacity) return capacity(state);
            return change_object(state, index, before, after, [](domain::AnnotationObject& changed, AnnotationEditorFacts& changed_facts) {
                changed.spline_knots.push_back({});
                changed_facts.selected_spline_segment = static_cast<std::uint16_t>(changed.spline_knots.size() - 1U);
            });
        }
        case domain::AnnotationSidebarCommand::SplineClose:
        case domain::AnnotationSidebarCommand::SplineReopen: {
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Spline)) return refused(state);
            const auto index = *before.selected_object;
            return change_object(state, index, before, after, [command](domain::AnnotationObject& changed, AnnotationEditorFacts&) {
                changed.spline_closed = command == domain::AnnotationSidebarCommand::SplineClose;
            });
        }
        case domain::AnnotationSidebarCommand::SplineDeleteKnot: {
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Spline) || !before.selected_spline_segment)
                return refused(state);
            const auto index = *before.selected_object;
            return change_object(state, index, before, after,
                                 [&before](domain::AnnotationObject& changed, AnnotationEditorFacts& changed_facts) {
                                     changed.spline_knots.erase(changed.spline_knots.begin() + *before.selected_spline_segment);
                                     changed_facts.selected_spline_segment.reset();
                                 });
        }
        case domain::AnnotationSidebarCommand::SkeletonSkip: {
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Skeleton)) return refused(state);
            const auto size = scene.objects[*before.selected_object].skeleton_nodes.size();
            if (size == 0U) return refused(state);
            after.selected_skeleton_joint =
                static_cast<std::uint16_t>((before.selected_skeleton_joint.value_or(static_cast<std::uint16_t>(size - 1U)) + 1U) % size);
            return commit(state, facts_entry(before, after));
        }
        case domain::AnnotationSidebarCommand::SkeletonReseed: {
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Skeleton) || !before.selected_skeleton_joint) {
                return refused(state);
            }
            const auto index = *before.selected_object;
            return change_object(state, index, before, after, [&before](domain::AnnotationObject& changed, AnnotationEditorFacts&) {
                auto& joint = changed.skeleton_nodes[*before.selected_skeleton_joint];
                joint.point = {};
                joint.visible = false;
            });
        }
        case domain::AnnotationSidebarCommand::SkeletonHide:
        case domain::AnnotationSidebarCommand::SkeletonShow: {
            if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Skeleton) || !before.selected_skeleton_joint)
                return refused(state);
            const auto index = *before.selected_object;
            return change_object(state, index, before, after,
                                 [&before, command](domain::AnnotationObject& changed, AnnotationEditorFacts&) {
                                     changed.skeleton_nodes[*before.selected_skeleton_joint].visible =
                                         command == domain::AnnotationSidebarCommand::SkeletonShow;
                                 });
        }
        case domain::AnnotationSidebarCommand::Undo:
        case domain::AnnotationSidebarCommand::Redo:
            break;
    }
    return refused(state);
}

[[nodiscard]] DocumentOutcome reduce_undo(DocumentState& state) { return apply_journal(state, JournalDirection::Undo); }

[[nodiscard]] DocumentOutcome reduce_redo(DocumentState& state) { return apply_journal(state, JournalDirection::Redo); }

class AnnotationDocument::Impl final {
   public:
    [[nodiscard]] DocumentResult Open(domain::AnnotationSceneContent content) {
        if (!content.valid()) return Result(DocumentOutcome::Rejected, "open");
        for (auto& object : content.objects)
            if (object.shape == domain::AnnotationShape::Mask) normalize_mask(object);
        if (content.palette.empty()) content.palette = domain::annotation_class_palette(content.categories.size());
        const auto outcome = reduce_open(state_, std::move(content));
        if (outcome == DocumentOutcome::Applied) capabilities_revision_ = 0U;
        return Result(outcome, "open");
    }
    [[nodiscard]] DocumentResult Pointer(const mmltk::controller::AnnotationPointer& pointer) {
        if (diagnostics_enabled_) {
            std::fprintf(stderr,
                         "{\"event\":\"annotation.pointer\",\"interaction_id\":%llu,\"sequence\":%llu,\"phase\":%u,\"target\":%d,\"x\":%."
                         "3f,\"y\":%.3f,\"radius\":%u}\n",
                         static_cast<unsigned long long>(pointer.interaction_id), static_cast<unsigned long long>(pointer.sequence),
                         static_cast<unsigned>(pointer.phase), pointer.target.object ? static_cast<int>(*pointer.target.object) : -1,
                         pointer.point.x, pointer.point.y, static_cast<unsigned>(pointer.brush_radius));
        }
        try {
            return Result(reduce_pointer(state_, pointer, pointer.brush_radius), "pointer");
        } catch (const std::length_error& error) {
            state_.pointer = {};
            return {.outcome = DocumentOutcome::Capacity, .detail = error.what()};
        }
    }
    void PeerClosed() noexcept { state_.pointer = {}; }
    [[nodiscard]] DocumentResult Edit(const mmltk::controller::AnnotationEdit& edit) {
        if (!current(state_)) return {.outcome = DocumentOutcome::Rejected, .detail = "Annotation document state is invalid"};
        state_.pointer = {};
        std::string_view operation;
        const auto result = std::visit(
            [this, &operation](const auto& value) -> DocumentOutcome {
                using Edit = std::remove_cvref_t<decltype(value)>;
                operation = std::meta::identifier_of(^^Edit);
                if constexpr (std::same_as<Edit, mmltk::controller::AnnotationToolEdit>)
                    return reduce_tool(state_, value.tool);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSetupEdit>)
                    return reduce_setup(state_, value.action);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationHoldEdit>)
                    return reduce_hold(state_, value.enabled);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSidebarEdit>)
                    return reduce_sidebar(state_, value.command);
                // CLEANUP-IGNORE: Every typed edit variant retains an explicit exhaustive reducer arm and domain
                // payload.
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationObjectEdit>)
                    return reduce_object(state_, value.object);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationCategoryEdit>)
                    return reduce_category(state_, value.category);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationClassEdit>) {
                    if (value.category >= state_.ui.scene.categories.size()) return refused(state_);
                    return change_editor_facts(state_, [&value](auto& editor) { editor.selected_category = value.category; });
                } else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSelectedObjectEdit>)
                    return reduce_object_facts(state_, value.category, value.enabled);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSplineEdit>)
                    return reduce_spline(state_, value.segment);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSplineHandleEdit>)
                    return reduce_handle(state_, value.handle, value.mode, value.point);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSkeletonEdit>)
                    return reduce_skeleton(state_, value.joint);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationMaskCleanupEdit>)
                    return reduce_mask_cleanup(state_, value.operation, value.radius);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationMaskColorsEdit>)
                    return reduce_mask_colors(state_, value.sup, value.nosup);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationSceneEdit>)
                    return reduce_scene(state_);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationUndoEdit>)
                    return reduce_undo(state_);
                else if constexpr (std::same_as<Edit, mmltk::controller::AnnotationRedoEdit>)
                    return reduce_redo(state_);
                else
                    static_assert(std::same_as<Edit, void>, "Unhandled AnnotationEdit alternative");
            },
            edit.value);
        return Result(result, operation);
    }
    [[nodiscard]] DocumentResult Save(const std::string_view destination) {
        if (!current(state_)) return {.outcome = DocumentOutcome::Rejected, .detail = "Annotation document state is invalid"};
        const auto effect = save_annotation_document(state_.ui, destination, next_save_generation_++);
        if (effect == DocumentSaveEffect::NotApplied) {
            state_.ui.save_status = domain::AnnotationSaveStatus::Failed;
            return {.outcome = DocumentOutcome::Rejected, .detail = "Annotation document save failed"};
        }
        state_.ui.save_status =
            effect == DocumentSaveEffect::Committed ? domain::AnnotationSaveStatus::Saved : domain::AnnotationSaveStatus::Uncertain;
        if (effect == DocumentSaveEffect::Committed) state_.ui.saved_revision = state_.ui.document_revision;
        return {.outcome = DocumentOutcome::Applied, .detail = {}};
    }
    [[nodiscard]] const domain::AnnotationUiState& ui() const noexcept { return state_.ui; }
    [[nodiscard]] bool ToolAvailable(domain::AnnotationTool tool, std::optional<std::uint16_t> target) const noexcept {
        return tool_applicable(state_.ui.scene, tool, target, true);
    }
    [[nodiscard]] std::size_t RenderObjectCount() const noexcept {
        return state_.ui.scene.objects.size() +
               ((state_.pointer.brush || state_.pointer.preview) && !state_.pointer.target.object ? 1U : 0U);
    }
    [[nodiscard]] const domain::AnnotationObject& RenderObjectAt(std::size_t index) const {
        const auto* preview = state_.pointer.brush ? &state_.brush : state_.pointer.preview ? &state_.preview : nullptr;
        if (preview && index == state_.pointer.target.object.value_or(static_cast<std::uint16_t>(state_.ui.scene.objects.size())))
            return *preview;
        return state_.ui.scene.objects.at(index);
    }

   private:
    [[nodiscard]] DocumentResult Result(const DocumentOutcome result, const std::string_view operation) {
        if (capabilities_revision_ != state_.ui.scene_revision) {
            state_.ui.tool_capabilities.clear();
            if (state_.ui.scene.document.valid()) {
                if (!tool_applicable(state_.ui.scene, state_.ui.editor.tool, state_.ui.editor.selected_object, false))
                    state_.ui.editor.tool = domain::AnnotationTool::Select;
                for (const auto entry : mmltk::frameworks::reflection::enum_entries<domain::AnnotationTool>())
                    state_.ui.tool_capabilities.push_back(
                        {entry.value, tool_applicable(state_.ui.scene, entry.value, state_.ui.editor.selected_object, false)});
            }
            capabilities_revision_ = state_.ui.scene_revision;
        }
        state_.ui.can_undo = !state_.undo.empty();
        state_.ui.can_redo = !state_.redo.empty();
        if (diagnostics_enabled_) {
            std::fprintf(stderr,
                         "{\"event\":\"annotation.document\",\"operation\":\"%.*s\",\"outcome\":%u,\"tool\":%u,\"document_revision\":%llu,"
                         "\"interaction_revision\":%llu,\"selected\":%d,\"reason\":\"%.*s\"}\n",
                         static_cast<int>(operation.size()), operation.data(), static_cast<unsigned>(result),
                         static_cast<unsigned>(state_.ui.editor.tool), static_cast<unsigned long long>(state_.ui.document_revision),
                         static_cast<unsigned long long>(state_.ui.interaction_revision),
                         state_.ui.editor.selected_object ? static_cast<int>(*state_.ui.editor.selected_object) : -1,
                         result == DocumentOutcome::Applied ? 0 : static_cast<int>(state_.rejection.size()), state_.rejection.data());
        }
        if (result == DocumentOutcome::Applied) return {.outcome = DocumentOutcome::Applied, .detail = {}};
        if (result == DocumentOutcome::Capacity)
            return {.outcome = DocumentOutcome::Capacity,
                    .detail = std::string{"Annotation "} + std::string{operation} + " exceeds bounded editor capacity"};
        return {.outcome = DocumentOutcome::Rejected,
                .detail = std::string{"Annotation "} + std::string{operation} + ": " + std::string{state_.rejection}};
    }

    const bool diagnostics_enabled_ = annotation_diagnostics_enabled();
    DocumentState state_;
    std::uint64_t next_save_generation_ = 1U;
    std::uint64_t capabilities_revision_ = 0U;
};

AnnotationDocument::AnnotationDocument() : impl_(std::make_unique<Impl>()) {}
AnnotationDocument::~AnnotationDocument() = default;
DocumentResult AnnotationDocument::Open(domain::AnnotationSceneContent content) { return impl_->Open(std::move(content)); }
DocumentResult AnnotationDocument::Pointer(const mmltk::controller::AnnotationPointer& pointer) { return impl_->Pointer(pointer); }
void AnnotationDocument::PeerClosed() noexcept { impl_->PeerClosed(); }
DocumentResult AnnotationDocument::Edit(const mmltk::controller::AnnotationEdit& edit) {
    try {
        return impl_->Edit(edit);
    } catch (const std::length_error& error) { return {.outcome = DocumentOutcome::Capacity, .detail = error.what()}; }
}
DocumentResult AnnotationDocument::Save(const std::string_view destination) { return impl_->Save(destination); }
const domain::AnnotationUiState& AnnotationDocument::ui() const noexcept { return impl_->ui(); }

bool AnnotationDocument::ToolAvailable(domain::AnnotationTool tool, std::optional<std::uint16_t> target) const noexcept {
    return impl_->ToolAvailable(tool, target);
}
std::size_t AnnotationDocument::RenderObjectCount() const noexcept { return impl_->RenderObjectCount(); }
const domain::AnnotationObject& AnnotationDocument::RenderObjectAt(std::size_t index) const { return impl_->RenderObjectAt(index); }

}  // namespace mmltk::controller::subsystems::annotation
