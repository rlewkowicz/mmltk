#pragma once
#include "src/backend/models/rfdetr/core/model.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
namespace mmltk::backend::models::rfdetr::testsupport {
[[nodiscard]] inline NativeCheckpointMetadata model_checkpoint_metadata(const NativeRfDetrModel& model, std::string source_kind, std::string source_path) {
 NativeCheckpointMetadata metadata;
 model_state_detail::visit_matching_members(metadata, model.config(), []<class Target, class Source>(Target& target, const Source& source) {
  if constexpr (!model_state_detail::is_optional<std::remove_cvref_t<Target>>) target = source;
 });
 metadata.class_layout = model.class_layout()->record();
 metadata.source_kind = std::move(source_kind);
 metadata.source_path = std::move(source_path);
 return metadata;
}
inline std::vector<NormalizedModelStateEntry> clone_normalized_model_state(const NativeRfDetrModel& module, const bool copy_to_cpu = false) {
 const auto parameters = module.named_parameters(true);
 const auto buffers = module.named_buffers(true);
 std::vector<NormalizedModelStateEntry> state;
 state.reserve(parameters.size() + buffers.size());
 const auto append = [&](const auto& item) {
  auto tensor = item.value().detach();
  if (copy_to_cpu) { tensor = tensor.cpu(); }
  state.push_back({item.key(), tensor.clone()});
 };
 for (const auto& parameter : parameters) { append(parameter); }
 for (const auto& buffer : buffers) { append(buffer); }
 return state;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
