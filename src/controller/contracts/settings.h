#pragma once
#include "src/frameworks/reflection/declaration_annotations.h"
#include <cstdint>
#include <vector>
#include "src/frameworks/gpu/device_inventory.h"
#include <string>
#include <type_traits>
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/gui_settings_states.h"
#include "src/controller/contracts/view_state.h"
#include "src/controller/contracts/workflows.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kSettingsUiStateByteBudget = 64U * 1024U;
struct ExploreSourceFact final {
 ExploreDatasetSource selection = ExploreDatasetSource::Train;
 MMLTK_MAX_PATH_BYTES std::string compiled_source;
 bool available = false;
 bool operator==(const ExploreSourceFact&) const = default;
};
struct SettingsUiState final {
 MMLTK_MAX_ITEMS(mmltk::frameworks::gpu::kCudaDeviceCapacity) std::vector<mmltk::frameworks::gpu::CudaDeviceFact> cuda_devices {};
 std::uint64_t revision = 0U;
 GuiSettingsState settings_state{};
 mmltk::backend::models::rfdetr::ExecutionFacts train_execution{};
 mmltk::backend::models::rfdetr::ExecutionFacts training_validation_execution{};
 mmltk::backend::models::rfdetr::ExecutionFacts validation_execution{};
 mmltk::backend::models::rfdetr::ExecutionFacts prediction_execution{};
 ExploreSourceFact explore_source{};
 MMLTK_MAX_PATH_BYTES std::string validation_source;
 bool operator==(const SettingsUiState&) const = default;
};
// This immutable fact separates installed settings from a default-constructed
// value. Compute systems materialize requests from an installed revision.
struct SettingsMaterializationFacts final {
 GuiSettingsState settings{};
 std::uint64_t revision = 0U;
 bool loaded = false;
 bool operator==(const SettingsMaterializationFacts&) const = default;
};
MMLTK_REFLECT_FIELDS(ExploreSourceFact)
MMLTK_REFLECT_FIELDS(SettingsUiState)
MMLTK_REFLECT_FIELDS(SettingsMaterializationFacts)
}  // namespace mmltk::controller::contracts
