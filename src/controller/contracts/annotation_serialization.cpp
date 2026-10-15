#include <cstddef>
#include <meta>
#include <vector>
#include "src/controller/contracts/annotation.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {
bool encode_annotation_persistence(const AnnotationUiState& state, std::vector<std::byte>& destination) noexcept {
 constexpr mmltk::frameworks::serialization::wire::Limits limits{.max_bytes = kAnnotationUiStateByteBudget, .max_items = kAnnotationUiStateByteBudget};
 return mmltk::frameworks::serialization::encode(state, destination, limits).has_value();
}
std::optional<AnnotationUiState> decode_annotation_persistence(std::span<const std::byte> bytes) noexcept {
 namespace serial = mmltk::frameworks::serialization;
 namespace wire = serial::wire;
 try {
  auto value = wire::decode({.first = bytes}, {.max_bytes = kAnnotationUiStateByteBudget, .max_items = kAnnotationUiStateByteBudget});
  if (!value) return {};
  const auto member = [](wire::Value& object, std::string_view key) -> wire::Value* {
   auto* fields = std::get_if<wire::Value::Object>(&object.storage);
   if (!fields) return nullptr;
   for (auto& [name, field] : *fields)
    if (name == key) return &field;
   return nullptr;
  };
  auto* scene = member(*value, "scene");
  if (!scene || !std::holds_alternative<wire::Value::Object>(scene->storage)) return {};
  // Documents written before joint schemas have the same scene and pixel identity.
  // Their boolean node visibility is converted below; no joint meaning is guessed.
  if (!member(*scene, "keypoint_schemas")) std::get<wire::Value::Object>(scene->storage).emplace_back("keypoint_schemas", wire::Value{wire::Value::Array{}});
  auto* objects = member(*scene, "objects");
  auto* array = objects ? std::get_if<wire::Value::Array>(&objects->storage) : nullptr;
  if (!array) return {};
  for (auto& object : *array) {
   auto* nodes = member(object, "skeleton_nodes");
   auto* joints = nodes ? std::get_if<wire::Value::Array>(&nodes->storage) : nullptr;
   if (!joints) return {};
   for (auto& joint : *joints) {
    auto* old = member(joint, "visible");
    if (!old) continue;
    if (member(joint, "visibility")) return {};
    auto* visible = std::get_if<bool>(&old->storage);
    if (!visible) return {};
    const auto converted = *visible ? mmltk::backend::data::catalog::KeypointVisibility::Visible : mmltk::backend::data::catalog::KeypointVisibility::Unannotated;
    for (auto& [name, field] : std::get<wire::Value::Object>(joint.storage))
     if (name == "visible") {
      name = "visibility";
      auto encoded = serial::reflected_value(converted);
      if (!encoded) return {};
      field = std::move(*encoded);
      break;
     }
   }
  }
  AnnotationUiState state;
  if (!serial::decode_into(state, *value) || !state.valid()) return {};
  return state;
 } catch (...) { return {}; }
}
}  // namespace mmltk::controller::contracts
