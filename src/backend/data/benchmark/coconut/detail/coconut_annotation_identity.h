#pragma once  // backend.data private implementation boundary
#include "src/backend/data/benchmark/coconut/detail/coconut_catalog.h"
#include <cstdint>
#include <string>
#include <string_view>
namespace mmltk::backend::data::benchmark_internal {
struct CoconutPhysicalImage;
inline constexpr std::uint32_t kCoconutRecoveryPolicy = 1;
[[nodiscard]] std::string coconut_image_input_identity(std::string_view annotations, const CoconutPhysicalImage&, std::string_view originals);
struct CoconutComponentMetadata {
 CoconutEdition edition = CoconutEdition::Base;
 CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
 std::string input_identity;
 std::uint32_t recovery_policy = 0;
 std::string original_annotation_identity;
 // Compile-local annotation lineage; persisted input_identity seals this plus physical dependencies.
 std::string annotation_input_identity{};
 std::uint64_t original_generation = 0;
};
[[nodiscard]] BenchmarkDatasetSource coconut_index_source(CoconutImageNamespace);
[[nodiscard]] std::string coconut_component_split(CoconutEdition, CoconutImageNamespace);
}  // namespace mmltk::backend::data::benchmark_internal
