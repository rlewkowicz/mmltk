#pragma once  // backend.data private implementation boundary
#include "benchmark_catalog.h"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
enum class CoconutEdition : std::uint8_t { Base, RelabeledValidation, Large, XLarge, ObjectsValidation };
enum class CoconutImageNamespace : std::uint8_t { CocoTrain, CocoUnlabeled, CocoValidation, Objects365V1, Objects365V2 };
struct CoconutReleaseComponent {
    CoconutEdition edition;
    std::string_view name;
    std::string_view revision;
    std::uint64_t expected_rows;
    std::vector<CatalogArtifact> annotations;
};
inline constexpr std::string_view kCoconutNormalizationRevision = "coconut-exact-rgb-rle-v1";
[[nodiscard]] std::span<const CoconutReleaseComponent> coconut_release_catalog();
[[nodiscard]] const CoconutReleaseComponent& coconut_release_component(CoconutEdition edition);
[[nodiscard]] const CatalogArtifact& coconut_unlabeled_images_artifact();
[[nodiscard]] const CatalogArtifact& coconut_validation_images_artifact();
[[nodiscard]] std::vector<CatalogArtifact> coconut_objects_training_artifacts(CoconutEdition edition);
[[nodiscard]] std::string_view coconut_namespace_name(CoconutImageNamespace source);
}  // namespace mmltk::backend::data::benchmark_internal
