#include "src/controller/presentation/annotation_palette.h"
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
#include "src/controller/subsystems/annotation/detail/annotation_render_state.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
namespace mmltk::controller::subsystems::annotation {
namespace {
using domain::AnnotationEditorFacts;
using domain::AnnotationSceneContent;
[[nodiscard]] bool annotation_diagnostics_enabled() noexcept {
 static const bool enabled = [] {
  const char* value = std::getenv("MMLTK_ANNOTATION_DIAGNOSTICS");
  return value != nullptr && value[0] == '1' && value[1] == '\0';
 }();
 return enabled;
}
inline constexpr std::size_t kHistoryCapacity = 32U;
enum class AnnotationPointerAction : std::uint8_t { Select, BoxDrag, PointPlace, Brush, Fill, SplineKnot, SkeletonJoint, HandleDrag, ColorSample };
struct PointerFacts final {
 bool active = false;
 std::uint64_t interaction_id = 0U;
 AnnotationPointerAction action = AnnotationPointerAction::Select;
 domain::AnnotationPointerTarget target{};
 domain::AnnotationTargetIdentity identity{};
 domain::AnnotationPoint origin{};
 domain::AnnotationPoint latest{};
 std::uint64_t sequence = 0U;
 bool brush = false;
 bool preview = false;
 std::uint16_t brush_radius = 0U;
 std::uint64_t created_identity = 0U;
};
struct JournalEntry final {
 struct Facts final {};
 struct Object final {
  std::uint16_t index = 0U;
  bool before_present = true;
  bool after_present = true;
  domain::AnnotationObject before{};
  domain::AnnotationObject after{};
  domain::AnnotationObjectIdentity before_identity{};
  domain::AnnotationObjectIdentity after_identity{};
 };
 struct Category final {
  std::uint16_t index = 0U;
  bool before_present = true;
  bool after_present = true;
  mmltk::backend::data::catalog::ClassName before{};
  mmltk::backend::data::catalog::ClassName after{};
 };
 struct Objects final {
  std::vector<domain::AnnotationObject> before;
  std::vector<domain::AnnotationObject> after;
  std::vector<domain::AnnotationObjectIdentity> before_identities{};
  std::vector<domain::AnnotationObjectIdentity> after_identities{};
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
// History remains owned by its source until admission and live payload preparation
// succeed. Direction changes references, never the retained entry's payloads.
static_assert(std::is_nothrow_move_constructible_v<JournalEntry> && std::is_nothrow_move_assignable_v<JournalEntry>);
static_assert(std::is_nothrow_move_constructible_v<domain::AnnotationObject> && std::is_nothrow_move_assignable_v<domain::AnnotationObject>);
static_assert(std::is_nothrow_move_constructible_v<domain::AnnotationObjectIdentity> && std::is_nothrow_move_assignable_v<domain::AnnotationObjectIdentity>);
struct JournalView final {
 const JournalEntry& entry;
 bool reverse = false;
 const AnnotationEditorFacts& before;
 [[nodiscard]] const AnnotationEditorFacts& after() const { return reverse ? entry.before : entry.after; }
 template <class T>
 [[nodiscard]] const T& Before(const T& first, const T& second) const {
  return reverse ? second : first;
 }
 template <class T>
 [[nodiscard]] const T& After(const T& first, const T& second) const {
  return reverse ? first : second;
 }
};
struct DocumentState final {
 domain::AnnotationUiState ui{};
 std::vector<domain::AnnotationObjectIdentity> identities;
 std::uint64_t next_identity = 1U;
 PointerFacts pointer{};
 domain::AnnotationObject brush;
 MaskRows brush_rows;
 domain::AnnotationObject preview;
 MaskScratch mask_scratch;
 std::deque<JournalEntry> undo{};
 std::deque<JournalEntry> redo{};
 std::string_view rejection = "The action requires a compatible selected object or handle";
};
[[nodiscard]] DocumentOutcome refused(DocumentState& state, const std::string_view reason = "The action requires a compatible selected object or handle") {
 state.rejection = reason;
 return DocumentOutcome::Rejected;
}
[[nodiscard]] DocumentOutcome capacity(const DocumentState&) { return DocumentOutcome::Capacity; }
// Open validates the imported scene; journal admission validates each committed mutation.
// Pointer previews never mutate this established document invariant.
[[nodiscard]] bool pointer_ready(const DocumentState& state) { return state.ui.scene.document.valid(); }
[[nodiscard]] bool current(const DocumentState& state) { return pointer_ready(state) && state.ui.valid(); }
[[nodiscard]] bool next_revision_available(const DocumentState& state) {
 return state.ui.document_revision != std::numeric_limits<std::uint64_t>::max() && state.ui.scene_revision != std::numeric_limits<std::uint64_t>::max() &&
        state.ui.interaction_revision != std::numeric_limits<std::uint64_t>::max();
}
[[nodiscard]] bool point_in_frame(const AnnotationSceneContent& scene, const domain::AnnotationPoint point) {
 return point.finite() && (!scene.frame_ready || (point.x >= 0.0F && point.y >= 0.0F && point.x <= scene.frame_width && point.y <= scene.frame_height));
}
[[nodiscard]] domain::AnnotationBox box_between(const domain::AnnotationPoint first, const domain::AnnotationPoint second) {
 return {.first = {.x = std::min(first.x, second.x), .y = std::min(first.y, second.y)},
         .second = {.x = std::max(first.x, second.x), .y = std::max(first.y, second.y)}};
}
[[nodiscard]] bool object_valid_for(const domain::AnnotationObject& object, const std::size_t categories, const AnnotationSceneContent& scene) {
 if (!object.valid() || object.category >= categories || !point_in_frame(scene, object.point) || !point_in_frame(scene, object.box.first) ||
     !point_in_frame(scene, object.box.second)) {
  return false;
 }
 return std::ranges::all_of(object.mask_points, [&scene](const auto& point) { return point_in_frame(scene, point); }) &&
        std::ranges::all_of(object.spline_knots,
                            [&scene](const auto& knot) {
                             return point_in_frame(scene, knot.point) && point_in_frame(scene, knot.in.point) && point_in_frame(scene, knot.out.point);
                            }) &&
        std::ranges::all_of(object.skeleton_nodes, [&scene](const auto& node) { return point_in_frame(scene, node.point); });
}
struct ObjectAfterView final {
 const AnnotationSceneContent& scene;
 const JournalView* entry = nullptr;
 [[nodiscard]] std::size_t size() const {
  if (const auto* objects = entry ? std::get_if<JournalEntry::Objects>(&entry->entry.mutation) : nullptr)
   return entry->After(objects->before, objects->after).size();
  const auto* mutation = entry ? std::get_if<JournalEntry::Object>(&entry->entry.mutation) : nullptr;
  if (!mutation) return scene.objects.size();
  if (!entry->Before(mutation->before_present, mutation->after_present) && entry->After(mutation->before_present, mutation->after_present))
   return scene.objects.size() + 1U;
  if (entry->Before(mutation->before_present, mutation->after_present) && !entry->After(mutation->before_present, mutation->after_present))
   return scene.objects.size() - 1U;
  return scene.objects.size();
 }
 [[nodiscard]] const domain::AnnotationObject* at(const std::size_t index) const {
  if (const auto* objects = entry ? std::get_if<JournalEntry::Objects>(&entry->entry.mutation) : nullptr)
   return index < entry->After(objects->before, objects->after).size() ? &entry->After(objects->before, objects->after)[index] : nullptr;
  const auto* mutation = entry ? std::get_if<JournalEntry::Object>(&entry->entry.mutation) : nullptr;
  if (!mutation) return index < scene.objects.size() ? &scene.objects[index] : nullptr;
  const std::size_t changed = mutation->index;
  if (entry->Before(mutation->before_present, mutation->after_present) && entry->After(mutation->before_present, mutation->after_present)) {
   if (index >= scene.objects.size()) return nullptr;
   return index == changed ? &entry->After(mutation->before, mutation->after) : &scene.objects[index];
  }
  if (!entry->Before(mutation->before_present, mutation->after_present) && entry->After(mutation->before_present, mutation->after_present)) {
   if (index >= size()) return nullptr;
   return index == changed ? &entry->After(mutation->before, mutation->after) : (index < changed ? &scene.objects[index] : &scene.objects[index - 1U]);
  }
  if (entry->Before(mutation->before_present, mutation->after_present) && !entry->After(mutation->before_present, mutation->after_present)) {
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
   return (object->shape == domain::AnnotationShape::Box || object->shape == domain::AnnotationShape::Mask) && *target.element < 4U;
  case domain::AnnotationHandleRole::Point: return object->shape == domain::AnnotationShape::Point && *target.element == 0U;
  case domain::AnnotationHandleRole::SplineKnot:
  case domain::AnnotationHandleRole::SplineInHandle:
  case domain::AnnotationHandleRole::SplineOutHandle: return object->shape == domain::AnnotationShape::Spline && *target.element < object->spline_knots.size();
  case domain::AnnotationHandleRole::SkeletonNode: return object->shape == domain::AnnotationShape::Skeleton && *target.element < object->skeleton_nodes.size();
 }
 return false;
}
[[nodiscard]] bool facts_valid_for(const AnnotationSceneContent& scene, const AnnotationEditorFacts& facts, const JournalView* entry = nullptr) {
 if (!mmltk::frameworks::reflection::enum_contains(facts.tool)) { return false; }
 ObjectAfterView objects{.scene = scene, .entry = entry};
 if (facts.selected_object && *facts.selected_object >= objects.size()) return false;
 const auto* category = entry ? std::get_if<JournalEntry::Category>(&entry->entry.mutation) : nullptr;
 const std::size_t category_count =
  scene.categories.size() +
  ((category && !entry->Before(category->before_present, category->after_present) && entry->After(category->before_present, category->after_present)) ? 1U
                                                                                                                                                      : 0U) -
  ((category && entry->Before(category->before_present, category->after_present) && !entry->After(category->before_present, category->after_present)) ? 1U
                                                                                                                                                      : 0U);
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
[[nodiscard]] bool entry_forward_valid(const DocumentState& state, const JournalView& entry) {
 if (entry.before != state.ui.editor || entry.entry.mutation.valueless_by_exception()) return false;
 const auto& scene = state.ui.scene;
 const bool mutation_valid = std::visit(
  [&scene, &entry](const auto& mutation) {
   using Mutation = std::remove_cvref_t<decltype(mutation)>;
   if constexpr (std::same_as<Mutation, JournalEntry::Facts>) {
    return true;
   } else if constexpr (std::same_as<Mutation, JournalEntry::Object>) {
    if (entry.Before(mutation.before_present, mutation.after_present) == entry.After(mutation.before_present, mutation.after_present)) {
     if (!entry.Before(mutation.before_present, mutation.after_present) || mutation.index >= scene.objects.size() ||
         scene.objects[mutation.index] != entry.Before(mutation.before, mutation.after))
      return false;
    } else if (entry.Before(mutation.before_present, mutation.after_present)) {
     if (mutation.index >= scene.objects.size() || scene.objects[mutation.index] != entry.Before(mutation.before, mutation.after)) return false;
    } else if (mutation.index > scene.objects.size()) {
     return false;
    }
    return !entry.After(mutation.before_present, mutation.after_present) ||
           object_valid_for(entry.After(mutation.before, mutation.after), scene.categories.size(), scene);
   } else if constexpr (std::same_as<Mutation, JournalEntry::Category>) {
    if (entry.Before(mutation.before_present, mutation.after_present) == entry.After(mutation.before_present, mutation.after_present)) return false;
    if (entry.Before(mutation.before_present, mutation.after_present)) {
     return !scene.categories.empty() && mutation.index + 1U == scene.categories.size() &&
            scene.categories[mutation.index] == entry.Before(mutation.before, mutation.after);
    }
    return mutation.index == scene.categories.size() && entry.After(mutation.before, mutation.after).valid();
   } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
    return entry.Before(mutation.before, mutation.after) == scene.objects;
   } else {
    return entry.Before(mutation.before_index, mutation.after_index) == scene.frame_index &&
           entry.Before(mutation.before_ready, mutation.after_ready) == scene.frame_ready;
   }
  },
  entry.entry.mutation);
 return mutation_valid && facts_valid_for(scene, entry.after(), &entry);
}
template <class T>
void prepare_append(std::vector<T>& values) {
 static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
 if (values.size() == values.capacity()) values.reserve(values.size() + std::max(values.size(), std::size_t{1U}));
}
[[nodiscard]] JournalEntry prepare_application(DocumentState& state, const JournalView& view, std::vector<domain::AnnotationColor>& palette) {
 JournalEntry prepared{.after = view.after()};
 std::visit(
  [&](const auto& mutation) {
   using Mutation = std::remove_cvref_t<decltype(mutation)>;
   auto& payload = prepared.mutation.emplace<Mutation>();
   if constexpr (std::same_as<Mutation, JournalEntry::Object>) {
    payload.index = mutation.index;
    payload.before_present = view.Before(mutation.before_present, mutation.after_present);
    payload.after_present = view.After(mutation.before_present, mutation.after_present);
    if (payload.after_present) {
     payload.after = view.After(mutation.before, mutation.after);
     payload.after_identity = view.After(mutation.before_identity, mutation.after_identity);
     if (!payload.before_present) {
      prepare_append(state.ui.scene.objects);
      prepare_append(state.identities);
     }
    }
   } else if constexpr (std::same_as<Mutation, JournalEntry::Category>) {
    payload.before_present = view.Before(mutation.before_present, mutation.after_present);
    payload.after_present = view.After(mutation.before_present, mutation.after_present);
    payload.after = view.After(mutation.before, mutation.after);
    if (payload.after_present) prepare_append(state.ui.scene.categories);
    palette =
     mmltk::controller::annotation_class_palette(state.ui.scene.categories.size() + (payload.after_present ? 1U : 0U) - (payload.before_present ? 1U : 0U));
   } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
    payload.after = view.After(mutation.before, mutation.after);
    payload.after_identities = view.After(mutation.before_identities, mutation.after_identities);
   } else if constexpr (std::same_as<Mutation, JournalEntry::Frame>) {
    payload.after_index = view.After(mutation.before_index, mutation.after_index);
    payload.after_ready = view.After(mutation.before_ready, mutation.after_ready);
   }
  },
  view.entry.mutation);
 return prepared;
}
void apply_forward(DocumentState& state, const JournalView& retained, JournalEntry& entry, std::vector<domain::AnnotationColor>& palette) {
 auto& scene = state.ui.scene;
 std::visit(
  [&scene, &state, &palette](auto& mutation) {
   using Mutation = std::remove_cvref_t<decltype(mutation)>;
   if constexpr (std::same_as<Mutation, JournalEntry::Object>) {
    if (!mutation.before_present && mutation.after_present) {
     scene.objects.insert(scene.objects.begin() + mutation.index, std::move(mutation.after));
     state.identities.insert(state.identities.begin() + mutation.index, std::move(mutation.after_identity));
    } else if (mutation.before_present && !mutation.after_present) {
     scene.objects.erase(scene.objects.begin() + mutation.index);
     state.identities.erase(state.identities.begin() + mutation.index);
    } else {
     scene.objects[mutation.index] = std::move(mutation.after);
     state.identities[mutation.index] = std::move(mutation.after_identity);
    }
   } else if constexpr (std::same_as<Mutation, JournalEntry::Category>) {
    if (!mutation.before_present && mutation.after_present)
     scene.categories.push_back(std::move(mutation.after));
    else
     scene.categories.pop_back();
    scene.palette = std::move(palette);
   } else if constexpr (std::same_as<Mutation, JournalEntry::Objects>) {
    scene.objects = std::move(mutation.after);
    state.identities = std::move(mutation.after_identities);
   } else if constexpr (std::same_as<Mutation, JournalEntry::Frame>) {
    scene.frame_index = mutation.after_index;
    scene.frame_ready = mutation.after_ready;
   }
  },
  entry.mutation);
 state.ui.editor = retained.after();
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
[[nodiscard]] std::size_t identity_element_count(const domain::AnnotationObject& object) noexcept {
 if (object.shape == domain::AnnotationShape::Spline) return object.spline_knots.size();
 if (object.shape == domain::AnnotationShape::Skeleton) return object.skeleton_nodes.size();
 return 0U;
}
[[nodiscard]] std::uint64_t allocate_identity(DocumentState& state) {
 if (state.next_identity == std::numeric_limits<std::uint64_t>::max()) throw std::length_error("Annotation runtime identity capacity exhausted");
 return state.next_identity++;
}
[[nodiscard]] domain::AnnotationObjectIdentity make_identity(DocumentState& state, const domain::AnnotationObject& object,
                                                             const std::uint64_t created_identity = 0U) {
 domain::AnnotationObjectIdentity identity{.object = created_identity ? created_identity : allocate_identity(state)};
 const auto count = identity_element_count(object);
 if (count != 0U) identity.elements.reserve(domain::kAnnotationGeometryCapacity);
 for (std::size_t index = 0; index < count; ++index) identity.elements.push_back(allocate_identity(state));
 return identity;
}
void identify_mutation(DocumentState& state, JournalEntry& entry) {
 if (auto* object = std::get_if<JournalEntry::Object>(&entry.mutation)) {
  if (object->before_present) object->before_identity = state.identities.at(object->index);
  if (!object->after_present) return;
  if (!object->before_present) {
   object->after_identity = make_identity(state, object->after, object->after_identity.object);
   return;
  }
  object->after_identity = object->before_identity;
  auto& elements = object->after_identity.elements;
  if (object->before.shape != object->after.shape) elements.clear();
  const auto count = identity_element_count(object->after);
  if (elements.size() > count) {
   // The sole element-removal operation removes the selected spline
   // knot. Keep the surviving knots' identities even at reused indices.
   if (object->before.shape != domain::AnnotationShape::Spline || elements.size() != count + 1U || !entry.before.selected_spline_segment)
    throw std::logic_error("Annotation element identity mutation is incomplete");
   elements.erase(elements.begin() + *entry.before.selected_spline_segment);
  }
  while (elements.size() < count) elements.push_back(allocate_identity(state));
 } else if (auto* objects = std::get_if<JournalEntry::Objects>(&entry.mutation)) {
  objects->before_identities = state.identities;
  objects->after_identities.reserve(objects->after.size());
  for (const auto& next_object : objects->after) objects->after_identities.push_back(make_identity(state, next_object));
 }
}
[[nodiscard]] DocumentOutcome commit(DocumentState& state, JournalEntry entry) {
 if (!current(state)) return refused(state);
 if (!entry_forward_valid(state, JournalView{entry, false, entry.before})) return refused(state);
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
 identify_mutation(state, entry);
 if (state.undo.size() == kHistoryCapacity) state.undo.pop_front();
 std::vector<domain::AnnotationColor> palette;
 auto prepared = prepare_application(state, JournalView{entry, false, entry.before}, palette);
 state.undo.push_back(std::move(entry));
 apply_forward(state, JournalView{state.undo.back(), false, state.ui.editor}, prepared, palette);
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
 const JournalView applied{source.back(), direction == JournalDirection::Undo, state.ui.editor};
 if (!entry_forward_valid(state, applied)) return refused(state);
 std::vector<domain::AnnotationColor> palette;
 auto prepared = prepare_application(state, applied, palette);
 // Allocate the deque node while the source still owns the complete history.
 destination.emplace_back();
 destination.back() = std::move(source.back());
 apply_forward(state, JournalView{destination.back(), direction == JournalDirection::Undo, state.ui.editor}, prepared, palette);
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
                                        domain::AnnotationObject after_object, const std::uint64_t created_identity = 0U) {
 return journal_entry(before, after,
                      JournalEntry::Object{.index = index,
                                           .before_present = before_present,
                                           .after_present = after_present,
                                           .before = before_object,
                                           .after = std::move(after_object),
                                           .after_identity = {.object = created_identity}});
}
template <class Mutate>
[[nodiscard]] DocumentOutcome change_object(DocumentState& state, const std::uint16_t index, const AnnotationEditorFacts& before, AnnotationEditorFacts after,
                                            Mutate&& mutate) {
 const domain::AnnotationObject& original = state.ui.scene.objects[index];
 domain::AnnotationObject changed = original;
 std::forward<Mutate>(mutate)(changed, after);
 // CLEANUP-IGNORE: Object mutation commit and category journal construction are distinct canonical variant
 // alternatives.
 return commit(state, object_entry(before, after, index, true, original, true, std::move(changed)));
}
[[nodiscard]] JournalEntry category_entry(const AnnotationEditorFacts& before, const AnnotationEditorFacts& after, const std::uint16_t index,
                                          const bool before_present, const mmltk::backend::data::catalog::ClassName& before_category, const bool after_present,
                                          const mmltk::backend::data::catalog::ClassName& after_category) {
 return journal_entry(
  before, after,
  JournalEntry::Category{.index = index, .before_present = before_present, .after_present = after_present, .before = before_category, .after = after_category});
}
[[nodiscard]] bool target_valid(const AnnotationSceneContent& scene, const AnnotationEditorFacts& editor, const domain::AnnotationPointerTarget& target) {
 (void)editor;
 return target_valid_for(target, ObjectAfterView{.scene = scene});
}
[[nodiscard]] bool action_matches_tool(const AnnotationPointerAction action, const domain::AnnotationTool tool) {
 switch (action) {
  case AnnotationPointerAction::Select: return true;
  case AnnotationPointerAction::BoxDrag: return tool == domain::AnnotationTool::Box;
  case AnnotationPointerAction::PointPlace: return tool == domain::AnnotationTool::Point;
  case AnnotationPointerAction::Brush: return tool == domain::AnnotationTool::MaskPaint || tool == domain::AnnotationTool::MaskErase;
  case AnnotationPointerAction::Fill: return tool == domain::AnnotationTool::MaskFill;
  case AnnotationPointerAction::SplineKnot: return tool == domain::AnnotationTool::Spline;
  case AnnotationPointerAction::SkeletonJoint: return tool == domain::AnnotationTool::Skeleton;
  case AnnotationPointerAction::HandleDrag: return true;
  case AnnotationPointerAction::ColorSample: return tool == domain::AnnotationTool::ColorSample;
 }
 return false;
}
[[nodiscard]] AnnotationPointerAction pointer_action(const domain::AnnotationTool tool, const domain::AnnotationPointerTarget& target) {
 if (target.role == domain::AnnotationHandleRole::SplineInHandle || target.role == domain::AnnotationHandleRole::SplineOutHandle)
  return AnnotationPointerAction::HandleDrag;
 switch (tool) {
  case domain::AnnotationTool::Select: return AnnotationPointerAction::Select;
  case domain::AnnotationTool::Box: return AnnotationPointerAction::BoxDrag;
  case domain::AnnotationTool::MaskPaint:
  case domain::AnnotationTool::MaskErase: return AnnotationPointerAction::Brush;
  case domain::AnnotationTool::MaskFill: return AnnotationPointerAction::Fill;
  case domain::AnnotationTool::Spline: return AnnotationPointerAction::SplineKnot;
  case domain::AnnotationTool::Point: return AnnotationPointerAction::PointPlace;
  case domain::AnnotationTool::Skeleton: return AnnotationPointerAction::SkeletonJoint;
  case domain::AnnotationTool::ColorSample: return AnnotationPointerAction::ColorSample;
 }
 return AnnotationPointerAction::Select;
}
[[nodiscard]] bool selected_shape(const AnnotationSceneContent& scene, const std::optional<std::uint16_t> selected, const domain::AnnotationShape shape) {
 return selected && *selected < scene.objects.size() && scene.objects[*selected].shape == shape;
}
[[nodiscard]] bool tool_applicable(const AnnotationSceneContent& scene, domain::AnnotationTool tool, std::optional<std::uint16_t> target,
                                   bool pointer_target) noexcept {
 if (!scene.frame_ready || !mmltk::frameworks::reflection::enum_contains(tool)) return false;
 if (target && *target >= scene.objects.size()) return false;
 const auto is_shape = [&](domain::AnnotationShape shape) { return target && scene.objects[*target].shape == shape; };
 switch (tool) {
  case domain::AnnotationTool::Select: return true;
  case domain::AnnotationTool::MaskErase:
  case domain::AnnotationTool::MaskFill:
  case domain::AnnotationTool::ColorSample: return is_shape(domain::AnnotationShape::Mask);
  case domain::AnnotationTool::MaskPaint: return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Mask));
  case domain::AnnotationTool::Box:
   return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Box) || is_shape(domain::AnnotationShape::Mask));
  case domain::AnnotationTool::Point: return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Point));
  case domain::AnnotationTool::Spline: return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Spline));
  case domain::AnnotationTool::Skeleton: return !scene.categories.empty() && (!pointer_target || !target || is_shape(domain::AnnotationShape::Skeleton));
 }
 return false;
}
[[nodiscard]] bool continues_pointer(const PointerFacts& active, const mmltk::controller::AnnotationPointer& request, const AnnotationPointerAction action) {
 return active.active && active.interaction_id == request.interaction_id && request.sequence > active.sequence && active.action == action &&
        active.target == request.target;
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
 float scale =
  knot.mode == domain::AnnotationSplineHandleMode::Mirrored ? 1.0F : std::hypot(opposite.point.x - knot.point.x, opposite.point.y - knot.point.y) / length;
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
  object.spline_knots[*target.element] = annotation_drag_knot(object.spline_knots[*target.element], {target, origin, point}, scene);
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
 } else if ((pointer.action == AnnotationPointerAction::Select || pointer.action == AnnotationPointerAction::HandleDrag) && pointer.target.object) {
  pointer.preview = true;
 }
}
}  // namespace
[[nodiscard]] DocumentOutcome reduce_pointer(DocumentState& state, const mmltk::controller::AnnotationPointer& request, const std::uint16_t brush_radius) {
 const auto tool = state.ui.editor.tool;
 const auto action = pointer_action(tool, request.target);
 const auto refuse_pointer = [&](const std::string_view reason = "Pointer coordinates or target are invalid for this tool") {
  if ((request.phase == domain::AnnotationPointerPhase::End || request.phase == domain::AnnotationPointerPhase::Cancel) && state.pointer.active &&
      state.pointer.interaction_id == request.interaction_id) {
   state.pointer = {};
  }
  return refused(state, reason);
 };
 if (!pointer_ready(state)) return refused(state, "Open an image before editing");
 if (!request.valid() || !mmltk::frameworks::reflection::enum_contains(request.phase) || !mmltk::frameworks::reflection::enum_contains(tool) ||
     request.interaction_id == 0U || request.sequence == 0U || !point_in_frame(state.ui.scene, request.point) ||
     !target_valid(state.ui.scene, state.ui.editor, request.target) ||
     (!action_matches_tool(action, tool) || !tool_applicable(state.ui.scene, tool, request.target.object, true))) {
  return refuse_pointer();
 }
 const auto before = state.ui.editor;
 auto after = before;
 if (request.phase == domain::AnnotationPointerPhase::Begin) {
  if (request.sequence != 1U || state.pointer.active) { return refused(state, "A new gesture requires sequence one and no active gesture"); }
  state.pointer = {.active = true,
                   .interaction_id = request.interaction_id,
                   .action = action,
                   .target = request.target,
                   .identity = request.identity,
                   .origin = request.point,
                   .latest = request.point,
                   .sequence = request.sequence,
                   .brush_radius = brush_radius};
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
   state.brush_rows.Assign(brush);
   brush.mask.runs.clear();
   state.brush_rows.Stroke(request.point, request.point, brush_radius, state.ui.scene.frame_width, state.ui.scene.frame_height,
                           tool == domain::AnnotationTool::MaskErase, state.mask_scratch);
   state.pointer.brush = true;
  }
  update_drag_preview(state, request.point);
  if (!request.target.object && (state.pointer.preview || state.pointer.brush)) state.pointer.created_identity = allocate_identity(state);
  return DocumentOutcome::Applied;
 }
 if (request.phase == domain::AnnotationPointerPhase::Update) {
  if (!continues_pointer(state.pointer, request, action)) { return refuse_pointer("Gesture identity, target or sequence does not match the active gesture"); }
  const bool changed = state.pointer.latest != request.point || state.pointer.brush_radius != brush_radius;
  if (state.pointer.brush && changed)
   state.brush_rows.Stroke(state.pointer.latest, request.point, brush_radius, state.ui.scene.frame_width, state.ui.scene.frame_height,
                           tool == domain::AnnotationTool::MaskErase, state.mask_scratch);
  if (changed) update_drag_preview(state, request.point);
  state.pointer.latest = request.point;
  state.pointer.sequence = request.sequence;
  state.pointer.brush_radius = brush_radius;
  return DocumentOutcome::Applied;
 }
 if (request.phase == domain::AnnotationPointerPhase::Cancel) {
  if (!continues_pointer(state.pointer, request, action)) { return refuse_pointer("Gesture identity, target or sequence does not match the active gesture"); }
  state.pointer = {};
  return DocumentOutcome::Applied;
 }
 if (!continues_pointer(state.pointer, request, action)) { return refuse_pointer("Gesture identity, target or sequence does not match the active gesture"); }
 after.tool = tool;
 const auto origin = state.pointer.origin;
 const auto target = state.pointer.target;
 const auto created_identity = state.pointer.created_identity;
 auto* brush = state.pointer.brush ? &state.brush : nullptr;
 if (brush) {
  state.brush_rows.Stroke(state.pointer.latest, request.point, brush_radius, state.ui.scene.frame_width, state.ui.scene.frame_height,
                          tool == domain::AnnotationTool::MaskErase, state.mask_scratch);
  state.brush_rows.Materialize(*brush);
 }
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
    return commit(state, object_entry(before, after, static_cast<std::uint16_t>(scene.objects.size()), false, {}, true, object, created_identity));
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
   if (!target.object && (scene.categories.empty() || scene.objects.size() >= domain::kAnnotationObjectCapacity)) return capacity(state);
   const auto index = target.object.value_or(static_cast<std::uint16_t>(scene.objects.size()));
   const domain::AnnotationObject existing = target.object ? scene.objects[index] : domain::AnnotationObject{};
   auto object = existing;
   if (!target.object) {
    object.category = after.selected_category.value_or(0U);
    object.name = domain::AnnotationText::From("annotation");
    switch (action) {
     case AnnotationPointerAction::PointPlace: object.shape = domain::AnnotationShape::Point; break;
     case AnnotationPointerAction::Brush:
     case AnnotationPointerAction::Fill: object.shape = domain::AnnotationShape::Mask; break;
     case AnnotationPointerAction::SplineKnot: object.shape = domain::AnnotationShape::Spline; break;
     case AnnotationPointerAction::SkeletonJoint: object.shape = domain::AnnotationShape::Skeleton; break;
     default: return refuse_pointer();
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
      object.skeleton_edges.push_back({static_cast<std::uint16_t>(object.skeleton_nodes.size() - 1), static_cast<std::uint16_t>(object.skeleton_nodes.size())});
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
    default: return refuse_pointer();
   }
   after.selected_object = index;
   if (object.shape != domain::AnnotationShape::Spline) after.selected_spline_segment.reset();
   if (object.shape != domain::AnnotationShape::Skeleton) after.selected_skeleton_joint.reset();
   return commit(state, object_entry(before, after, index, target.object.has_value(), existing, true, object, created_identity));
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
 if (std::ranges::any_of(scene.categories, [&](const auto& existing) { return existing.value == category.view(); }))
  return refused(state, "Class name is already present");
 if (scene.categories.size() >= domain::kAnnotationCategoryCapacity) return capacity(state);
 auto after = state.ui.editor;
 after.selected_category = static_cast<std::uint16_t>(scene.categories.size());
 return commit(
  state, category_entry(state.ui.editor, after, static_cast<std::uint16_t>(scene.categories.size()), false, {}, true, {.value = std::string{category.view()}}));
}
[[nodiscard]] DocumentOutcome reduce_object_facts(DocumentState& state, const std::uint16_t category, const bool enabled) {
 if (!current(state) || !state.ui.editor.selected_object || category >= state.ui.scene.categories.size()) return refused(state);
 const auto index = *state.ui.editor.selected_object;
 return change_object(state, index, state.ui.editor, state.ui.editor, [category, enabled](domain::AnnotationObject& object, AnnotationEditorFacts&) {
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
[[nodiscard]] DocumentOutcome reduce_handle(DocumentState& state, const domain::AnnotationHandleRole handle, const domain::AnnotationSplineHandleMode mode,
                                            const domain::AnnotationPoint point) {
 const auto& scene = state.ui.scene;
 if (!current(state) || !selected_shape(scene, state.ui.editor.selected_object, domain::AnnotationShape::Spline) || !state.ui.editor.selected_spline_segment ||
     !point_in_frame(scene, point) || (handle != domain::AnnotationHandleRole::SplineInHandle && handle != domain::AnnotationHandleRole::SplineOutHandle) ||
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
[[nodiscard]] DocumentOutcome reduce_mask_cleanup(DocumentState& state, const domain::AnnotationMaskCleanup operation, const std::uint16_t cleanup_radius) {
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
[[nodiscard]] DocumentOutcome reduce_mask_colors(DocumentState& state, const domain::AnnotationColorRange& sup, const domain::AnnotationColorRange& nosup) {
 if (!current(state) || !selected_shape(state.ui.scene, state.ui.editor.selected_object, domain::AnnotationShape::Mask) ||
     // CLEANUP-IGNORE: Mask-color validation and object-category validation guard different domain invariants.
     !sup.valid() || !nosup.valid())
  return refused(state);
 const auto index = *state.ui.editor.selected_object;
 return change_object(state, index, state.ui.editor, state.ui.editor, [&sup, &nosup](domain::AnnotationObject& object, AnnotationEditorFacts&) {
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
 return commit(state, {.before = state.ui.editor, .after = after, .mutation = JournalEntry::Objects{.before = state.ui.scene.objects, .after = {}}});
}
[[nodiscard]] DocumentOutcome reduce_open(DocumentState& state, domain::AnnotationSceneContent content) {
 if (state.ui.document_revision == std::numeric_limits<std::uint64_t>::max() || !content.valid()) return DocumentOutcome::Rejected;
 DocumentState replacement{};
 const std::uint64_t revision = state.ui.document_revision + 1U;
 replacement.ui.scene = std::move(content);
 replacement.identities.reserve(domain::kAnnotationObjectCapacity);
 for (const auto& object : replacement.ui.scene.objects) replacement.identities.push_back(make_identity(replacement, object));
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
   if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Box) || before.tool == domain::AnnotationTool::Box) return refused(state);
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
   if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Spline) || !before.selected_spline_segment) return refused(state);
   const auto index = *before.selected_object;
   return change_object(state, index, before, after, [&before](domain::AnnotationObject& changed, AnnotationEditorFacts& changed_facts) {
    changed.spline_knots.erase(changed.spline_knots.begin() + *before.selected_spline_segment);
    changed_facts.selected_spline_segment.reset();
   });
  }
  case domain::AnnotationSidebarCommand::SkeletonSkip: {
   if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Skeleton)) return refused(state);
   const auto size = scene.objects[*before.selected_object].skeleton_nodes.size();
   if (size == 0U) return refused(state);
   after.selected_skeleton_joint = static_cast<std::uint16_t>((before.selected_skeleton_joint.value_or(static_cast<std::uint16_t>(size - 1U)) + 1U) % size);
   return commit(state, facts_entry(before, after));
  }
  case domain::AnnotationSidebarCommand::SkeletonReseed: {
   if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Skeleton) || !before.selected_skeleton_joint) { return refused(state); }
   const auto index = *before.selected_object;
   return change_object(state, index, before, after, [&before](domain::AnnotationObject& changed, AnnotationEditorFacts&) {
    auto& joint = changed.skeleton_nodes[*before.selected_skeleton_joint];
    joint.point = {};
    joint.visible = false;
   });
  }
  case domain::AnnotationSidebarCommand::SkeletonHide:
  case domain::AnnotationSidebarCommand::SkeletonShow: {
   if (!selected_shape(scene, before.selected_object, domain::AnnotationShape::Skeleton) || !before.selected_skeleton_joint) return refused(state);
   const auto index = *before.selected_object;
   return change_object(state, index, before, after, [&before, command](domain::AnnotationObject& changed, AnnotationEditorFacts&) {
    changed.skeleton_nodes[*before.selected_skeleton_joint].visible = command == domain::AnnotationSidebarCommand::SkeletonShow;
   });
  }
  case domain::AnnotationSidebarCommand::Undo:
  case domain::AnnotationSidebarCommand::Redo: break;
 }
 return refused(state);
}
[[nodiscard]] DocumentOutcome reduce_undo(DocumentState& state) { return apply_journal(state, JournalDirection::Undo); }
[[nodiscard]] DocumentOutcome reduce_redo(DocumentState& state) { return apply_journal(state, JournalDirection::Redo); }
class AnnotationDocument::Impl final {
public:
 [[nodiscard]] DocumentResult Open(domain::AnnotationSceneContent content) {
  if (!content.valid()) return Result(DocumentOutcome::Rejected, "open");
  try {
   const mmltk::backend::data::catalog::ClassCatalog catalog(mmltk::backend::data::catalog::OrderedClassCatalog{content.categories});
  } catch (...) { return Result(DocumentOutcome::Rejected, "open"); }
  for (auto& object : content.objects)
   if (object.shape == domain::AnnotationShape::Mask) normalize_mask(object);
  if (content.palette.empty()) content.palette = mmltk::controller::annotation_class_palette(content.categories.size());
  const auto outcome = reduce_open(state_, std::move(content));
  if (outcome == DocumentOutcome::Applied) {
   capabilities_revision_ = 0U;
   // Open restarts scene revisions from the document revision, so a
   // replacement source may reuse the prior scene revision.
   render_scene_.reset();
  }
  return Result(outcome, "open");
 }
 [[nodiscard]] DocumentResult Pointer(const mmltk::controller::AnnotationPointer& pointer) {
  const auto scene_revision = state_.ui.scene_revision;
  const auto before = state_.pointer;
  const auto changed = [&] {
   const auto& after = state_.pointer;
   const bool was_preview = before.brush || before.preview;
   const bool is_preview = after.brush || after.preview;
   return scene_revision != state_.ui.scene_revision || was_preview != is_preview ||
          (is_preview && (before.latest != after.latest || before.brush_radius != after.brush_radius || before.interaction_id != after.interaction_id));
  };
  if (diagnostics_enabled_) {
   std::fprintf(stderr,
                "{\"event\":\"annotation.pointer\",\"interaction_id\":%llu,\"sequence\":%llu,\"phase\":%u,\"target\":%d,\"x\":%."
                "3f,\"y\":%.3f,\"radius\":%u}\n",
                static_cast<unsigned long long>(pointer.interaction_id), static_cast<unsigned long long>(pointer.sequence),
                static_cast<unsigned>(pointer.phase), pointer.target.object ? static_cast<int>(*pointer.target.object) : -1, pointer.point.x, pointer.point.y,
                static_cast<unsigned>(pointer.brush_radius));
  }
  try {
   auto result = Result(reduce_pointer(state_, pointer, pointer.brush_radius), "pointer");
   result.render_changed = changed();
   return result;
  } catch (const std::length_error& error) {
   state_.pointer = {};
   return {.outcome = DocumentOutcome::Capacity, .detail = error.what(), .render_changed = changed()};
  }
 }
 [[nodiscard]] bool HasPreview() const noexcept { return state_.pointer.brush || state_.pointer.preview; }
 bool PeerClosed() noexcept {
  const bool changed = HasPreview();
  state_.pointer = {};
  return changed;
 }
 [[nodiscard]] DocumentResult Edit(const mmltk::controller::AnnotationEdit& edit) {
  if (!current(state_)) return {.outcome = DocumentOutcome::Rejected, .detail = "Annotation document state is invalid"};
  const auto scene_revision = state_.ui.scene_revision;
  const bool preview = state_.pointer.brush || state_.pointer.preview;
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
  auto reduced = Result(result, operation);
  reduced.render_changed = preview || scene_revision != state_.ui.scene_revision;
  return reduced;
 }
 [[nodiscard]] DocumentResult Save(const std::string_view destination) {
  if (!current(state_)) return {.outcome = DocumentOutcome::Rejected, .detail = "Annotation document state is invalid"};
  const auto effect = save_annotation_document(state_.ui, destination, next_save_generation_++);
  if (effect == DocumentSaveEffect::NotApplied) {
   state_.ui.save_status = domain::AnnotationSaveStatus::Failed;
   return {.outcome = DocumentOutcome::Rejected, .detail = "Annotation document save failed"};
  }
  state_.ui.save_status = effect == DocumentSaveEffect::Committed ? domain::AnnotationSaveStatus::Saved : domain::AnnotationSaveStatus::Uncertain;
  if (effect == DocumentSaveEffect::Committed) state_.ui.saved_revision = state_.ui.document_revision;
  return {.outcome = DocumentOutcome::Applied, .detail = {}};
 }
 [[nodiscard]] const domain::AnnotationUiState& ui() const noexcept { return state_.ui; }
 [[nodiscard]] domain::AnnotationPointerTarget HitTarget(domain::AnnotationPoint point) const noexcept {
  using Tool = domain::AnnotationTool;
  using Shape = domain::AnnotationShape;
  using Role = domain::AnnotationHandleRole;
  const auto& scene = state_.ui.scene;
  const auto selected = state_.ui.editor.selected_object;
  const auto tool = state_.ui.editor.tool;
  if (tool == Tool::Box || tool == Tool::Point) return {};
  if (tool != Tool::Select) {
   if (!selected || *selected >= scene.objects.size()) return {};
   const auto shape = scene.objects[*selected].shape;
   if (((tool == Tool::MaskPaint || tool == Tool::MaskErase || tool == Tool::MaskFill || tool == Tool::ColorSample) && shape == Shape::Mask) ||
       (tool == Tool::Spline && shape == Shape::Spline) || (tool == Tool::Skeleton && shape == Shape::Skeleton))
    return {.object = selected};
   return {};
  }
  const auto near = [point](domain::AnnotationPoint other) { return std::hypot(point.x - other.x, point.y - other.y) <= 6.0F; };
  if (selected && *selected < scene.objects.size()) {
   const auto& object = scene.objects[*selected];
   const auto handle = [selected](std::size_t element, Role role) {
    return domain::AnnotationPointerTarget{selected, static_cast<std::uint16_t>(element), role};
   };
   if (object.shape == Shape::Box || object.shape == Shape::Mask) {
    const auto& box = object.box;
    const std::array<domain::AnnotationPoint, 4U> corners{{{box.first.x - 5.0F, box.first.y - 5.0F},
                                                           {box.second.x + 4.0F, box.first.y - 5.0F},
                                                           {box.second.x + 4.0F, box.second.y + 4.0F},
                                                           {box.first.x - 5.0F, box.second.y + 4.0F}}};
    for (std::size_t index = 0U; index != corners.size(); ++index)
     if (near(corners[index])) return handle(index, Role::BoxCorner);
   }
   if (object.shape == Shape::Point && near(object.point)) return handle(0U, Role::Point);
   for (std::size_t index = 0U; index != object.spline_knots.size(); ++index) {
    const auto& knot = object.spline_knots[index];
    if (near(knot.point)) return handle(index, Role::SplineKnot);
    if (knot.in.enabled && near(knot.in.point)) return handle(index, Role::SplineInHandle);
    if (knot.out.enabled && near(knot.out.point)) return handle(index, Role::SplineOutHandle);
   }
   for (std::size_t index = 0U; index != object.skeleton_nodes.size(); ++index)
    if (object.skeleton_nodes[index].visible && near(object.skeleton_nodes[index].point)) return handle(index, Role::SkeletonNode);
  }
  for (std::size_t index = scene.objects.size(); index-- != 0U;) {
   const auto& object = scene.objects[index];
   if (!object.enabled) continue;
   bool hit = false;
   switch (object.shape) {
    case Shape::Box:
     hit = point.x >= object.box.first.x - 3.0F && point.x <= object.box.second.x + 3.0F && point.y >= object.box.first.y - 3.0F &&
           point.y <= object.box.second.y + 3.0F;
     break;
    case Shape::Mask:
     hit = std::ranges::any_of(object.mask.runs, [point](const auto& run) {
      return run.row == static_cast<std::uint16_t>(std::clamp(point.y, 0.0F, 65535.0F)) && point.x >= run.first && point.x < run.last + 1.0F;
     });
     break;
    case Shape::Point: hit = near(object.point); break;
    case Shape::Spline: hit = std::ranges::any_of(object.spline_knots, [&](const auto& knot) { return near(knot.point); }); break;
    case Shape::Skeleton: hit = std::ranges::any_of(object.skeleton_nodes, [&](const auto& node) { return node.visible && near(node.point); }); break;
   }
   if (hit) {
    domain::AnnotationPointerTarget target{.object = static_cast<std::uint16_t>(index)};
    if (object.shape == Shape::Point) {
     target.element = 0U;
     target.role = Role::Point;
    }
    return target;
   }
  }
  return {};
 }
 [[nodiscard]] bool ResolveTarget(mmltk::controller::AnnotationPointer& pointer) const noexcept {
  if (pointer.phase == domain::AnnotationPointerPhase::Begin) {
   pointer.target = HitTarget(pointer.point);
   if (!target_valid(state_.ui.scene, state_.ui.editor, pointer.target)) return false;
   pointer.identity = {};
   if (pointer.target.object) {
    const auto& identity = state_.identities[*pointer.target.object];
    pointer.identity.object = identity.object;
    if (pointer.target.element) {
     const auto role = *pointer.target.role;
     pointer.identity.element = role == domain::AnnotationHandleRole::BoxCorner || role == domain::AnnotationHandleRole::Point
                                 ? static_cast<std::uint64_t>(*pointer.target.element) + 1U
                                 : identity.elements[*pointer.target.element];
    }
   }
   return true;
  }
  if (!pointer.target.valid()) return false;
  if (!pointer.target.object) return pointer.identity.object == 0U && pointer.identity.element == 0U;
  if (!pointer.identity.object) return false;
  // Ordinary edits cancel the active reducer gesture, so its accepted
  // current-index mapping remains valid for every ordered update. Only a
  // new gesture searches the bounded identity vector.
  if (pointer.phase != domain::AnnotationPointerPhase::Begin && state_.pointer.active && pointer.interaction_id == state_.pointer.interaction_id &&
      pointer.identity == state_.pointer.identity && pointer.target.role == state_.pointer.target.role) {
   pointer.target = state_.pointer.target;
   return true;
  }
  const auto found = std::ranges::find(state_.identities, pointer.identity.object, &domain::AnnotationObjectIdentity::object);
  if (found == state_.identities.end()) return false;
  pointer.target.object = static_cast<std::uint16_t>(found - state_.identities.begin());
  if (!pointer.target.element) return pointer.identity.element == 0U && !pointer.target.role;
  if (!pointer.target.role || !pointer.identity.element) return false;
  const auto role = *pointer.target.role;
  if (role == domain::AnnotationHandleRole::BoxCorner || role == domain::AnnotationHandleRole::Point) {
   if (pointer.identity.element != static_cast<std::uint64_t>(*pointer.target.element) + 1U) return false;
  } else {
   const auto element = std::ranges::find(found->elements, pointer.identity.element);
   if (element == found->elements.end()) return false;
   pointer.target.element = static_cast<std::uint16_t>(element - found->elements.begin());
  }
  return target_valid(state_.ui.scene, state_.ui.editor, pointer.target);
 }
 [[nodiscard]] bool ToolAvailable(domain::AnnotationTool tool, std::optional<std::uint16_t> target) const noexcept {
  return tool_applicable(state_.ui.scene, tool, target, true);
 }
 void CaptureRender(AnnotationRenderState& target) {
  target.scene.reset();
  target.identities.reset();
  if (!render_scene_ || render_document_revision_ != state_.ui.document_revision) {
   if (!state_.ui.valid()) throw contracts::UnavailableError("Annotation document state is invalid");
   auto available = std::ranges::find_if(render_storage_, [](const auto& scene) { return !scene || scene.use_count() == 1; });
   if (available == render_storage_.end()) throw std::logic_error("Annotation render description custody exceeded");
   if (!*available) *available = std::make_shared<RenderBacking>();
   (*available)->scene = state_.ui.scene;
   (*available)->identities = state_.identities;
   render_scene_ = *available;
   render_document_revision_ = state_.ui.document_revision;
  }
  target.scene = {render_scene_, &render_scene_->scene};
  target.identities = {render_scene_, &render_scene_->identities};
  target.editor = state_.ui.editor;
  target.scene_revision = state_.ui.scene_revision;
  target.preview_object.reset();
  target.drag.reset();
  target.brush_rows.reset();
  target.preview_materialized = false;
  target.preview_identity = state_.pointer.created_identity;
  if (state_.pointer.brush || state_.pointer.preview) {
   target.preview_object = state_.pointer.target.object.value_or(static_cast<std::uint16_t>(state_.ui.scene.objects.size()));
   if (state_.pointer.brush) {
    target.preview = state_.brush;
    target.brush_rows = state_.brush_rows;
   } else if (state_.pointer.target.object) {
    target.drag = AnnotationDragPreview{state_.pointer.target, state_.pointer.origin, state_.pointer.latest};
   } else {
    target.preview = state_.preview;
   }
  }
 }

private:
 [[nodiscard]] DocumentResult Result(const DocumentOutcome result, const std::string_view operation) {
  if (capabilities_revision_ != state_.ui.scene_revision) {
   state_.ui.tool_capabilities.clear();
   if (state_.ui.scene.document.valid()) {
    if (!tool_applicable(state_.ui.scene, state_.ui.editor.tool, state_.ui.editor.selected_object, false))
     state_.ui.editor.tool = domain::AnnotationTool::Select;
    for (const auto entry : mmltk::frameworks::reflection::enum_entries<domain::AnnotationTool>())
     state_.ui.tool_capabilities.push_back({entry.value, tool_applicable(state_.ui.scene, entry.value, state_.ui.editor.selected_object, false)});
   }
   capabilities_revision_ = state_.ui.scene_revision;
  }
  state_.ui.can_undo = !state_.undo.empty();
  state_.ui.can_redo = !state_.redo.empty();
  if (diagnostics_enabled_) {
   std::fprintf(stderr,
                "{\"event\":\"annotation.document\",\"operation\":\"%.*s\",\"outcome\":%u,\"tool\":%u,\"document_revision\":%llu,"
                "\"interaction_revision\":%llu,\"selected\":%d,\"reason\":\"%.*s\"}\n",
                static_cast<int>(operation.size()), operation.data(), static_cast<unsigned>(result), static_cast<unsigned>(state_.ui.editor.tool),
                static_cast<unsigned long long>(state_.ui.document_revision), static_cast<unsigned long long>(state_.ui.interaction_revision),
                state_.ui.editor.selected_object ? static_cast<int>(*state_.ui.editor.selected_object) : -1,
                result == DocumentOutcome::Applied ? 0 : static_cast<int>(state_.rejection.size()), state_.rejection.data());
  }
  if (result == DocumentOutcome::Applied) return {.outcome = DocumentOutcome::Applied, .detail = {}};
  if (result == DocumentOutcome::Capacity)
   return {.outcome = DocumentOutcome::Capacity, .detail = std::string{"Annotation "} + std::string{operation} + " exceeds bounded editor capacity"};
  return {.outcome = DocumentOutcome::Rejected, .detail = std::string{"Annotation "} + std::string{operation} + ": " + std::string{state_.rejection}};
 }
 const bool diagnostics_enabled_ = annotation_diagnostics_enabled();
 DocumentState state_;
 // The document's current content plus the three scratch/pending/active
 // descriptions bound immutable version custody. Pool-only values are safe
 // to refill: no renderer can acquire them, and releasing a description never
 // destroys its vectors. No wait or scene-sized retirement enters input.
 struct RenderBacking final {
  domain::AnnotationSceneContent scene;
  std::vector<domain::AnnotationObjectIdentity> identities;
 };
 std::array<std::shared_ptr<RenderBacking>, 4U> render_storage_{};
 std::shared_ptr<const RenderBacking> render_scene_;
 std::uint64_t render_document_revision_ = 0U;
 std::uint64_t next_save_generation_ = 1U;
 std::uint64_t capabilities_revision_ = 0U;
};
AnnotationDocument::AnnotationDocument() : impl_(std::make_unique<Impl>()) {}
AnnotationDocument::~AnnotationDocument() = default;
DocumentResult AnnotationDocument::Open(domain::AnnotationSceneContent content) { return impl_->Open(std::move(content)); }
DocumentResult AnnotationDocument::Pointer(const mmltk::controller::AnnotationPointer& pointer) { return impl_->Pointer(pointer); }
bool AnnotationDocument::ResolveTarget(mmltk::controller::AnnotationPointer& pointer) const noexcept { return impl_->ResolveTarget(pointer); }
bool AnnotationDocument::PeerClosed() noexcept { return impl_->PeerClosed(); }
DocumentResult AnnotationDocument::Edit(const mmltk::controller::AnnotationEdit& edit) {
 const bool preview = impl_->HasPreview();
 const auto revision = impl_->ui().scene_revision;
 try {
  return impl_->Edit(edit);
 } catch (const std::length_error& error) {
  return {.outcome = DocumentOutcome::Capacity, .detail = error.what(), .render_changed = preview || revision != impl_->ui().scene_revision};
 }
}
DocumentResult AnnotationDocument::Save(const std::string_view destination) { return impl_->Save(destination); }
const domain::AnnotationUiState& AnnotationDocument::ui() const noexcept { return impl_->ui(); }
bool AnnotationDocument::ToolAvailable(domain::AnnotationTool tool, std::optional<std::uint16_t> target) const noexcept {
 return impl_->ToolAvailable(tool, target);
}
void AnnotationDocument::CaptureRender(AnnotationRenderState& target) { impl_->CaptureRender(target); }
}  // namespace mmltk::controller::subsystems::annotation
namespace mmltk::controller {
contracts::AnnotationSplineKnot annotation_drag_knot(contracts::AnnotationSplineKnot knot, const AnnotationDragPreview& drag,
                                                     const contracts::AnnotationSceneContent& scene) {
 if (drag.target.role == contracts::AnnotationHandleRole::SplineKnot) {
  const float dx = drag.point.x - knot.point.x, dy = drag.point.y - knot.point.y;
  for (auto* handle : {&knot.in, &knot.out}) {
   handle->point.x = std::clamp(handle->point.x + dx, 0.0F, static_cast<float>(scene.frame_width));
   handle->point.y = std::clamp(handle->point.y + dy, 0.0F, static_cast<float>(scene.frame_height));
  }
  knot.point = drag.point;
 } else if (drag.target.role == contracts::AnnotationHandleRole::SplineInHandle || drag.target.role == contracts::AnnotationHandleRole::SplineOutHandle) {
  subsystems::annotation::set_spline_handle(knot, *drag.target.role, drag.point, scene);
 }
 return knot;
}
void materialize_annotation_drag(contracts::AnnotationObject& object, const AnnotationDragPreview& drag, const contracts::AnnotationSceneContent& scene,
                                 subsystems::annotation::MaskScratch& scratch) {
 subsystems::annotation::drag_object(object, drag.target, drag.origin, drag.point, scene, scratch);
}
}  // namespace mmltk::controller
