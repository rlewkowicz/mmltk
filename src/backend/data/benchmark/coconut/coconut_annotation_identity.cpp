#include "src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_inventory.h"
#include "src/common/io/file_digest.h"
#include <cstdint>
#include <span>
#include <nlohmann/json.hpp>
namespace mmltk::backend::data::benchmark_internal {
std::string coconut_image_input_identity(std::string_view annotations, const CoconutPhysicalImage& physical, std::string_view originals) {
 const auto material = nlohmann::json{
  {"domain", "coconut-image-labels-v1"}, {"annotations", annotations},
  {"physical", {physical.source == CoconutImageNamespace::CocoUnlabeled ? CoconutImageNamespace::CocoTrain : physical.source, physical.image_id}},
  {"recovery_policy", originals.empty() ? 0 : kCoconutRecoveryPolicy}, {"originals", originals}
 }.dump();
 return mmltk::common::io::sha256_hex(mmltk::common::io::sha256_bytes(std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size())));
}
BenchmarkDatasetSource coconut_index_source(CoconutImageNamespace source) {
 return source == CoconutImageNamespace::Objects365V1 || source == CoconutImageNamespace::Objects365V2 ? BenchmarkDatasetSource::kObjects365V2 : BenchmarkDatasetSource::kCoco2017;
}
std::string coconut_component_split(CoconutEdition edition, CoconutImageNamespace source) {
 return "coconut-" + std::to_string(static_cast<unsigned>(edition)) + "-" + std::to_string(static_cast<unsigned>(source));
}
}  // namespace mmltk::backend::data::benchmark_internal
