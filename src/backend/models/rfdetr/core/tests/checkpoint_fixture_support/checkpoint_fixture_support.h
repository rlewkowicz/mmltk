#pragma once
#include <array>
#include <vector>
#include <utility>
#include "src/backend/models/rfdetr/core/model_state.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "src/backend/models/rfdetr/core/class_layout.h"
namespace mmltk::backend::models::rfdetr::testsupport {
inline void set_synthetic_model_state(DecodedNativeModelState& state, std::vector<NormalizedModelStateEntry> entries) { state.replace_entries(std::move(entries)); }
struct ParityFixtureCase;
// Synthetic fixture vocabulary is explicit test data, never artifact inference.
[[nodiscard]] inline ModelClassLayout synthetic_training_layout(std::size_t foreground_count) {
 std::vector<std::string> names;
 names.reserve(foreground_count);
 for (std::size_t index = 0; index < foreground_count; ++index) names.push_back("fixture-class-" + std::to_string(index));
 return native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog(std::move(names)));
}
[[nodiscard]] inline NativeCheckpointMetadata synthetic_training_metadata() {
 NativeCheckpointMetadata value;
 value.class_layout = synthetic_training_layout(1);
 value.preset_name = "rf-detr-nano";
 value.source_kind = "native-training-test";
 value.source_path = "seed.pt";
 value.num_classes = 2;
 value.num_queries = 2;
 value.num_select = 2;
 return value;
}
void write_minimal_upstream_checkpoint(const std::filesystem::path& path, const ParityFixtureCase& fixture);
void log_fixture_phase(const char* test_name, std::size_t index, std::size_t total, const char* phase, const char* preset_name);
}  // namespace mmltk::backend::models::rfdetr::testsupport
