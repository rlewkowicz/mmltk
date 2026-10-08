#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace mmltk::tools::review {

using Json = nlohmann::ordered_json;

std::size_t physicalLines(std::string_view source);
// files is an ordered JSON array of {path, source, language, sha256}; rawReport
// has the raw_matches and classified_context collections from rawCpdReport.
Json buildReview(const Json& files, const Json& rawReport);
std::string renderReviewMarkdown(const Json& review);

}  // namespace mmltk::tools::review
