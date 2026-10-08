#pragma once

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::tools::include_audit {

using Json = nlohmann::ordered_json;
using SourceReader = std::function<std::string(const std::string&)>;
using PathAvailable = std::function<bool(const std::string&)>;

// Objects retain JavaScript Map insertion order. Contexts map source paths to
// arrays; sources map paths to UTF-8 text; coverage maps PCH paths to arrays of
// header names. Source offsets (including scan tokens/regions) count UTF-16
// code units, matching the shared lexer and original audit reports.
std::vector<std::string> commandWords(std::string_view command);
Json compilationContexts(const Json& entries, std::string_view root,
                         const SourceReader& reader = {}, const PathAvailable& available = {});
Json scanIncludes(std::string_view source);
Json attributeInclude(const Json& include, std::string_view path,
                      const std::set<std::string>& paths, const Json& contexts,
                      std::string_view root);
Json repeatedBundles(const Json& files);
Json pchReplacement(std::string_view path, std::string_view source, const Json& scan,
                    const Json& contexts, const Json& coverage);
Json buildIncludeReport(const std::vector<std::string>& paths, const Json& sources,
                        const Json& contexts, std::string_view root);
std::string renderIncludeMarkdown(const Json& report);
Json parseIncludeArgs(const std::vector<std::string>& args);
int main(const std::vector<std::string>& args);

}  // namespace mmltk::tools::include_audit
