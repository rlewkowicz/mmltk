#pragma once  // backend.data private implementation boundary
#include "benchmark_catalog.h"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
enum class CoconutEdition : std::uint8_t { Base = 0, RelabeledValidation = 1, Large = 2, XLarge = 3, ObjectsValidation = 4 };
enum class CoconutImageNamespace : std::uint8_t { CocoTrain = 0, CocoUnlabeled = 1, CocoValidation = 2, Objects365V1 = 3, Objects365V2 = 4 };
// Persisted inventory and completion identities retain these numeric meanings.
static_assert(static_cast<std::uint8_t>(CoconutEdition::Base) == 0 &&
              static_cast<std::uint8_t>(CoconutEdition::RelabeledValidation) == 1 &&
              static_cast<std::uint8_t>(CoconutEdition::Large) == 2 &&
              static_cast<std::uint8_t>(CoconutEdition::XLarge) == 3 &&
              static_cast<std::uint8_t>(CoconutEdition::ObjectsValidation) == 4);
static_assert(static_cast<std::uint8_t>(CoconutImageNamespace::CocoTrain) == 0 &&
              static_cast<std::uint8_t>(CoconutImageNamespace::CocoUnlabeled) == 1 &&
              static_cast<std::uint8_t>(CoconutImageNamespace::CocoValidation) == 2 &&
              static_cast<std::uint8_t>(CoconutImageNamespace::Objects365V1) == 3 &&
              static_cast<std::uint8_t>(CoconutImageNamespace::Objects365V2) == 4);
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
