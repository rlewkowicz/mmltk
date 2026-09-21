#pragma once
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <cstdint>
#include <inplace_vector>
#include <string>
#include <type_traits>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/workflows.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::contracts {
inline constexpr std::size_t kMaxSettingsPathBytes = 512U;
inline constexpr std::size_t kMaxSettingsFlatValueBytes = 16U * 1024U;
inline constexpr std::size_t kMaxSettingsFlatValueItems = 64U;
struct SettingsValueUpdate final {
 [[= mmltk::frameworks::reflection::MaxBytes{kMaxSettingsPathBytes}]] std::string path;
 [[= mmltk::frameworks::reflection::MaxBytes{kMaxSettingsFlatValueBytes}]]
  [[= mmltk::frameworks::reflection::MaxItems{kMaxSettingsFlatValueItems}]] mmltk::frameworks::serialization::wire::FlatValue value;
};
inline constexpr std::size_t kMaxSettingsUpdates = 64U;
struct[[= reflection::all_feature_scope()]] SettingsUpdateRequest final {
 [[= mmltk::frameworks::reflection::MaxItems{
  kMaxSettingsUpdates}]][[= reflection::direct::SettingsUpdateValues{}]] std::inplace_vector<SettingsValueUpdate, kMaxSettingsUpdates>
  updates;
};
struct[[= reflection::all_feature_scope()]] SettingsResetRequest final {};
MMLTK_REFLECT_FIELDS(SettingsValueUpdate)
MMLTK_REFLECT_FIELDS(SettingsUpdateRequest)
MMLTK_REFLECT_FIELDS(SettingsResetRequest)
}  // namespace mmltk::controller::contracts
