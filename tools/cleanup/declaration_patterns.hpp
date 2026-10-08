#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cpd_patterns.hpp"

namespace mmltk::tools::declarations {

using Json = nlohmann::ordered_json;

inline constexpr std::string_view ANNOTATION_HEADER =
    "src/frameworks/reflection/declaration_annotations.h";
inline constexpr std::string_view DECLARATION_SURFACE =
    "src/frameworks/reflection/reflected_declarations.h";
extern const std::map<std::string, std::string> ANNOTATION_MACROS;
extern const std::set<std::string> MACRO_NAMES;

struct Options {
  std::set<std::string> macroConflicts;
};

std::vector<std::size_t> lineStarts(std::string_view source);
Json sourceOccurrence(std::string_view path, std::string_view source, const Json& occurrence);
Json sourceOccurrence(std::string_view path, std::string_view source, const Json& occurrence,
                      const std::vector<std::size_t>& lines);
Json preprocessorRegions(std::string_view source);
Json preprocessorRegions(std::string_view source, const std::vector<cpd::Token>& tokens);
std::vector<std::string> conflictingMacros(std::string_view path, std::string_view source);
std::set<std::string> inventoryMacroConflicts(const std::map<std::string, std::string>& sources);
std::optional<std::size_t> invocationEnd(const cpd::SourceIndex& index, std::size_t start);
Json classifyDeclarations(std::string_view path, std::string_view source,
                          const Options& options = {});
Json formatDeclarationSource(std::string_view path, std::string_view source,
                             const Options& options = {});

}  // namespace mmltk::tools::declarations
