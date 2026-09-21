#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include "src/controller/contracts/workflows.h"
#include "src/frameworks/reflection/field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/backend/models/catalog/artifacts.h"
namespace mmltk::controller::contracts {
using mmltk::backend::models::catalog::ModelArtifactInputKind;
enum class ModelSelectionSource : std::uint8_t {
 Canonical = 0,
 Custom = 1,
};
inline constexpr std::size_t kModelArtifactCapacity = 4096U;
struct ModelSelectionKey final {
 FeatureId workflow = FeatureId::Train;
 ModelSelectionSource source = ModelSelectionSource::Canonical;
 ModelArtifactInputKind input = ModelArtifactInputKind::None;
 [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumNameBytes}]] std::string preset;
 std::uint32_t resolution = 0U;
 [[= mmltk::frameworks::reflection::MaxBytes{kModelArtifactCapacity}]] std::string class_layout_path{};
 [[nodiscard]] bool valid() const noexcept;
 bool operator==(const ModelSelectionKey&) const = default;
};
// A draft projection is not an admitted model; its key may be incomplete.
struct ModelSettingsProjection final {
 ModelSelectionKey key{};
 [[= mmltk::frameworks::reflection::MaxBytes{kModelArtifactCapacity}]] std::string artifact;
 bool compatible = false;
 bool export_build_tensorrt = false;
 bool operator==(const ModelSettingsProjection&) const = default;
};
MMLTK_REFLECT_FIELDS(ModelSelectionKey)
MMLTK_REFLECT_FIELDS(ModelSettingsProjection)
MMLTK_REFLECT_ENUM(ModelSelectionSource)
}  // namespace mmltk::controller::contracts
