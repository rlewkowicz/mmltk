#include "native_catalog.h"
#include <nlohmann/json.hpp>
#include <type_traits>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/backend/data/catalog/class_catalog.h"
namespace {
template <class T>
nlohmann::json project(const T& value) {
 using namespace mmltk::frameworks::reflection;
 if constexpr (std::is_enum_v<T>) return enum_name(value);
 else if constexpr (std::is_arithmetic_v<T> || std::is_convertible_v<T, std::string_view>) return value;
 else if constexpr (requires { value.begin(); value.end(); }) {
  auto result = nlohmann::json::array();
  for (const auto& item : value) result.push_back(project(item));
  return result;
 } else {
  auto result = nlohmann::json::object();
  visit_materialized_members<T>([&]<class Declaration>(const auto& field) { result[field.member_name] = project(value.*Declaration::pointer); });
  return result;
 }
}
}
std::string rfdetr_native_catalog_json() {
 using namespace mmltk::backend::models::rfdetr;
 auto result = nlohmann::json::object();
 result["presets"] = project(kPresetCatalog);
 result["foreground_capacity"] = mmltk::backend::data::catalog::kClassCatalogCapacity;
 result["output_slot_capacity"] = kMaximumClassOutputSlots;
 return result.dump();
}
