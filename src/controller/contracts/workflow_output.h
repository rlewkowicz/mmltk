#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <vector>
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kWorkflowArtifactCapacity = 8U;
struct WorkflowOutputSelection final {
 bool automatic = true;
 MMLTK_MAX_PATH_BYTES[[= reflection::FileDialog<"Select output directory", "Directories", "*">{.mode = FileDialogMode::OpenFolder}]] std::string directory;
 bool operator==(const WorkflowOutputSelection&) const = default;
};
// Collections remain bounded independently of the number of processed images.
// A recoverable in-progress video is never advertised as a completed artifact.
struct WorkflowOutputFacts final {
 MMLTK_MAX_PATH_BYTES std::string directory;
 MMLTK_MAX_ITEMS(kWorkflowArtifactCapacity) MMLTK_MAX_PATH_BYTES std::vector<std::filesystem::path> artifacts;
 MMLTK_MAX_PATH_BYTES std::string samples_directory;
 std::uint64_t completed_samples = 0;
 MMLTK_MAX_PATH_BYTES std::string recent_sample;
 MMLTK_MAX_PATH_BYTES std::string partial_video;
 bool operator==(const WorkflowOutputFacts&) const = default;
};
[[nodiscard]] inline std::filesystem::path workflow_output_root(const WorkflowOutputSelection& selection, FeatureId workflow) {
 if (!selection.automatic) return selection.directory;
 switch (workflow) {
  case FeatureId::Train: return "./output/train";
  case FeatureId::Validate: return "./output/validate";
  case FeatureId::Predict: return "./output/predict";
  case FeatureId::Export: return "./output/export";
  default: throw std::invalid_argument("workflow has no run output");
 }
}
MMLTK_REFLECT_FIELDS(WorkflowOutputSelection)
MMLTK_REFLECT_FIELDS(WorkflowOutputFacts)
}  // namespace mmltk::controller::contracts
