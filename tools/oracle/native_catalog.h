#pragma once
#include <string>
#include <string_view>
// Native reflected declarations, compiled independently of the Python bridge.
[[nodiscard]] std::string rfdetr_native_catalog_json();
[[nodiscard]] std::string rfdetr_native_configuration_json(std::string_view preset, int groups);
