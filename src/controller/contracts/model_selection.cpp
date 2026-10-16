#include "src/controller/contracts/model_selection.h"
namespace mmltk::controller::contracts {
bool ModelSelectionKey::valid() const noexcept {
 return (!from_scratch || (workflow == FeatureId::Train && source == ModelSelectionSource::Canonical && input == ModelArtifactInputKind::Weights && class_layout_path.empty())) &&
        model_selection_compatible(workflow, source, input) && !preset.empty() && preset.size() <= mmltk::frameworks::reflection::kMaximumNameBytes && resolution != 0U &&
        class_layout_path.size() <= kModelArtifactCapacity && class_layout_path.find('\0') == std::string::npos;
}
}  // namespace mmltk::controller::contracts
