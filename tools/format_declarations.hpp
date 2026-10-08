#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::tools::formatter {

using Json = nlohmann::ordered_json;

Json parseFormatArgs(const std::vector<std::string>& args);
Json declarationFormatReport(const std::vector<std::string>& inventory,
                             const std::map<std::string, std::string>& sources,
                             const Json& options = Json::object());
int runFormatter(const Json& options);
int main(const std::vector<std::string>& args);

}  // namespace mmltk::tools::formatter
