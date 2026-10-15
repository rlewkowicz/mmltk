#include "native_catalog.h"
#include <nlohmann/json.hpp>
#include <type_traits>
#include <stdexcept>
#include "src/backend/models/rfdetr/contract/preset_catalog.h"
#include "src/backend/models/rfdetr/contract/model_config.h"
#include "src/backend/models/rfdetr/contract/class_layout.h"
#include "src/backend/data/catalog/class_catalog.h"
namespace {
template <class T>
nlohmann::json project(const T& value) {
 using namespace mmltk::frameworks::reflection;
 if constexpr (std::is_enum_v<T>)
  return enum_name(value);
 else if constexpr (std::is_arithmetic_v<T> || std::is_convertible_v<T, std::string_view>)
  return value;
 else if constexpr (requires {
                     value.begin();
                     value.end();
                    }) {
  auto result = nlohmann::json::array();
  for (const auto& item : value) result.push_back(project(item));
  return result;
 } else {
  auto result = nlohmann::json::object();
  visit_materialized_members<T>([&]<class Declaration>(const auto& field) { result[field.member_name] = project(value.*Declaration::pointer); });
  return result;
 }
}
}  // namespace
std::string rfdetr_native_catalog_json() {
 using namespace mmltk::backend::models::rfdetr;
 auto result = nlohmann::json::object();
 result["presets"] = project(kPresetCatalog);
 result["foreground_capacity"] = mmltk::backend::data::catalog::kClassCatalogCapacity;
 result["output_slot_capacity"] = kMaximumClassOutputSlots;
 return result.dump();
}
std::string rfdetr_native_configuration_json(const std::string_view preset, const int groups) {
 using namespace mmltk::backend::models::rfdetr;
 const auto* row = find_preset_catalog_entry(preset);
 if (!row) throw std::invalid_argument("unknown RF-DETR oracle preset: " + std::string(preset));
 auto config = native_config_from_preset(*row);
 config.group_detr = groups;
 if (!native_model_config_valid(config)) throw std::invalid_argument("invalid RF-DETR oracle configuration: " + std::string(preset));
 const auto result = project(config).dump();
 if (result.size() > 65536U) throw std::invalid_argument("RF-DETR oracle configuration exceeds checkpoint byte bound");
 return result;
}
