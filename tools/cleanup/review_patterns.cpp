// Human review evidence only: lexical shapes do not establish semantic
// equivalence, extraction permission, source registration, or rewrite rules.
export module mmltk.tools.cleanup.review_patterns;
import std;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.cpp_locale;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cleanup.declaration_patterns;
export {
 namespace mmltk::tools::review {
 using Json = mmltk::tools::json;
 std::size_t physicalLines(std::string_view source);
 // files is an ordered JSON array of {path, source, language, sha256}; rawReport
 // has the raw_matches and classified_context collections from rawCpdReport.
 Json buildReview(const Json& files, const Json& rawReport);
 std::string renderReviewMarkdown(const Json& review);
 }  // namespace mmltk::tools::review
}
namespace mmltk::tools::review {
namespace {
const std::string& stringAt(const Json& value, std::string_view key) { return value.at(std::string(key)).get_ref<const std::string&>(); }
bool identifier(std::string_view text) {
 const auto initial = [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; };
 return !text.empty() && initial(text.front()) && std::all_of(text.begin() + 1, text.end(), [&](char ch) { return initial(ch) || (ch >= '0' && ch <= '9'); });
}
struct Entry {
 std::string kind;
 Json occurrence;
 std::vector<std::string> values;
 std::vector<std::string> shape;
 std::string key;
};
Entry shapedOccurrence(std::string kind, Json occurrence) {
 static constexpr std::array fixed{"template", "typename", "class", "struct", "const", "constexpr", "consteval", "static", "inline", "bool", "void", "true", "false", "requires", "return"};
 Entry result{std::move(kind), std::move(occurrence), {}, {}, {}};
 const auto tokens = cpd::tokenizeCpp(stringAt(result.occurrence, "text"));
 result.values.reserve(tokens.size());
 result.shape.reserve(tokens.size());
 for (const auto& token : tokens) {
  const auto& text = token.text;
  result.values.push_back(text);
  if (token.kind == "literal")
   result.shape.emplace_back("$literal");
  else if (!text.empty() && text.front() >= '0' && text.front() <= '9')
   result.shape.emplace_back("$number");
  else if (identifier(text) && std::find(fixed.begin(), fixed.end(), text) == fixed.end())
   result.shape.emplace_back("$identifier");
  else
   result.shape.push_back(text);
 }
 // Qualification reuse requires the same prefix, not merely equal depth.
 if (result.kind == "qualification") {
  for (std::size_t i = 0; i + 1 < result.values.size(); ++i) result.shape[i] = result.values[i];
 }
 result.key = '[' + json_dump(result.kind) + ",[";
 for (std::size_t i = 0; i < result.shape.size(); ++i) {
  if (i) result.key += ',';
  result.key += json_dump(result.shape[i]);
 }
 result.key += "]]";
 return result;
}
std::vector<Entry> structuralOccurrences(const std::string& path, const std::string& source) {
 const cpd::SourceIndex index(source);
 const auto& tokens = index.tokens;
 std::vector<Entry> occurrences;
 const auto text = [&](std::ptrdiff_t at) -> std::string_view { return at >= 0 && static_cast<std::size_t>(at) < tokens.size() ? tokens[at].text : std::string_view{}; };
 const auto memberAccess = [](std::string_view value) { return value == "." || value == "->"; };
 const auto add = [&](std::string kind, std::size_t start, std::size_t end) {
  if (end <= start || end - start > 2048) return;
  auto occurrence = declarations::sourceOccurrence(path, source, index.occurrence(path, start, end), index.lines);
  occurrence["owner"] = nullptr;
  if (index.owners[start] >= 0)
   occurrence["owner"] = index.functions[index.owners[start]].name;
  else if (index.recordOwners[start] >= 0)
   occurrence["owner"] = index.records[index.recordOwners[start]].name;
  occurrences.push_back(shapedOccurrence(std::move(kind), std::move(occurrence)));
 };
 for (std::size_t i = 0; i < tokens.size(); ++i) {
  const auto at = static_cast<std::ptrdiff_t>(i);
  if (text(at) == "template" && text(at + 1) == "<") {
   const auto limit = i + std::min<std::size_t>(tokens.size() - i, 2048);
   for (auto end = i + 2; end < limit; ++end) {
    if (tokens[end].text == ";") break;
    if (tokens[end].text != "struct") continue;
    const auto name = text(static_cast<std::ptrdiff_t>(end) + 1);
    if (!name.starts_with("has_") && !name.starts_with("is_") && !name.ends_with("Annotation")) break;
    while (end < limit && tokens[end].text != "{" && tokens[end].text != ";") ++end;
    const auto close = index.pairs.find(end);
    if (close != index.pairs.end() && close->second > end && text(static_cast<std::ptrdiff_t>(close->second) + 1) == ";") { add("trait_declaration", i, close->second + 2); }
    break;
   }
  }
  if (text(at) == "=" && memberAccess(text(at - 2)) && identifier(text(at - 1))) {
   const auto member = text(at - 1);
   auto end = i + 1;
   const auto limit = i + std::min<std::size_t>(tokens.size() - i, 128);
   while (end < limit && tokens[end].text != ";" && tokens[end].text != "{") ++end;
   bool sameName = false;
   for (auto right = i + 2; right < end; ++right) {
    if (tokens[right].text == member && memberAccess(tokens[right - 1].text)) {
     sameName = true;
     break;
    }
   }
   if (text(static_cast<std::ptrdiff_t>(end)) == ";" && sameName) {
    if (i < 3) throw std::invalid_argument("same-name mapping has no receiver token");
    auto start = i - 3;
    while (start > 0 && tokens[start - 1].text != ";" && tokens[start - 1].text != "{" && tokens[start - 1].text != "}" && i - start < 128) --start;
    add("same_name_mapping", start, end + 1);
   }
  }
  if (tokens[i].kind == "literal" && text(at - 1) == "(" && text(at + 1) == ",") {
   const std::string_view literal = tokens[i].text;
   if (literal.size() < 3 || literal.front() != '"' || literal.back() != '"') continue;
   const auto name = literal.substr(1, literal.size() - 2);
   const auto close = index.pairs.find(i - 1);
   if (identifier(name) && close != index.pairs.end() && close->second > i && close->second - i < 128 && text(static_cast<std::ptrdiff_t>(close->second) - 1) == name &&
       memberAccess(text(static_cast<std::ptrdiff_t>(close->second) - 2)) && text(static_cast<std::ptrdiff_t>(close->second) + 1) == ";") {
    add("same_name_mapping", i < 2 ? 0 : i - 2, close->second + 2);
   }
  }
 }
 return occurrences;
}
std::string occurrenceKey(const Json& occurrence) { return stringAt(occurrence, "path") + ':' + occurrence.at("start_offset").dump() + ':' + occurrence.at("end_offset").dump(); }
std::size_t unionLines(const std::vector<const Json*>& occurrences) {
 std::map<std::string, std::vector<std::pair<std::size_t, std::size_t>>> byPath;
 for (const auto* occurrence : occurrences) { byPath[stringAt(*occurrence, "path")].emplace_back(occurrence->at("start").get<std::size_t>(), occurrence->at("end").get<std::size_t>()); }
 std::size_t count = 0;
 for (auto& [path, ranges] : byPath) {
  std::sort(ranges.begin(), ranges.end());
  std::size_t end = 0;
  for (const auto& [start, last] : ranges) {
   const auto covered = std::max(end, start - 1);
   if (last > covered) count += last - covered;
   end = std::max(end, last);
  }
 }
 return count;
}
void associateRawMatches(const std::vector<Json*>& occurrences, const Json& rawMatches) {
 struct Event {
  std::size_t at;
  bool open;
  bool candidate;
  std::size_t value;
 };
 std::map<std::string, std::vector<Event>> byPath;
 const auto add = [&](const Json& occurrence, bool candidate, std::size_t value) {
  auto& events = byPath[stringAt(occurrence, "path")];
  events.push_back({occurrence.at("start_offset").get<std::size_t>(), true, candidate, value});
  events.push_back({occurrence.at("end_offset").get<std::size_t>(), false, candidate, value});
 };
 std::vector<std::set<std::size_t>> ids(occurrences.size());
 for (std::size_t i = 0; i < occurrences.size(); ++i) add(*occurrences[i], true, i);
 for (const auto& match : rawMatches) {
  for (const auto& occurrence : match.at("occurrences")) {
   if (byPath.contains(stringAt(occurrence, "path"))) add(occurrence, false, match.at("id").get<std::size_t>());
  }
 }
 for (auto& [path, events] : byPath) {
  // Close before open at a shared boundary: touching spans do not overlap.
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.at != b.at ? a.at < b.at : a.open < b.open; });
  std::set<std::size_t> candidates;
  std::map<std::size_t, std::ptrdiff_t> raw;
  for (const auto& event : events) {
   if (event.candidate) {
    if (event.open) {
     for (const auto& [id, count] : raw) ids[event.value].insert(id);
     candidates.insert(event.value);
    } else
     candidates.erase(event.value);
   } else if (event.open) {
    ++raw[event.value];
    for (const auto candidate : candidates) ids[candidate].insert(event.value);
   } else {
    if (--raw[event.value] == 0) raw.erase(event.value);
   }
  }
 }
 for (std::size_t i = 0; i < occurrences.size(); ++i) (*occurrences[i])["raw_match_ids"] = ids[i];
}
// std::regex has ASCII whitespace; the source expressions use ECMAScript /u
// whitespace. Normalize just that character class before matching evidence.
std::string evidenceText(std::string_view source) {
 auto units = cpd::toUtf16(source);
 for (auto& ch : units) {
  if (ch >= 0x80 && text::whitespace(ch)) ch = u' ';
 }
 return cpd::fromUtf16(units);
}
Json testEvidence(const std::string& path, const std::string& source, const std::string& language) {
 static const std::regex dedicated(R"((?:^|/)(?:tests?|fixtures)(?:/|\.)|(?:\.test\.|(?:^|/)test_[^/]+|(?:^|/)tests\.rs$))");
 static const std::regex rust(R"(#\s*\[\s*(?:test|cfg\s*\(\s*test\s*\))\s*\])");
 static const std::regex cpp(R"(\b(?:TEST_CASE|TEMPLATE_TEST_CASE|SCENARIO)\s*\()");
 bool embedded = false;
 if (language == "rust" || language == "cpp") {
  const auto normalized = evidenceText(source);
  embedded = std::regex_search(normalized, language == "rust" ? rust : cpp);
 }
 return {{"dedicated", std::regex_search(path, dedicated)}, {"embedded", embedded}};
}
std::string markdownEscape(std::string_view text, bool preformatted) {
 std::string result;
 for (const char ch : text) {
  if (preformatted) {
   if (ch == '&')
    result += "&amp;";
   else if (ch == '<')
    result += "&lt;";
   else if (ch == '>')
    result += "&gt;";
   else
    result += ch;
  } else if (ch == '|' || ch == '`' || ch == '<' || ch == '>' || ch == '\r' || ch == '\n') {
   result += "&#" + std::to_string(static_cast<unsigned char>(ch)) + ';';
  } else
   result += ch;
 }
 return result;
}
std::string joinedNumbers(const Json& values, std::size_t limit) {
 std::string result;
 for (std::size_t i = 0; i < std::min(values.size(), limit); ++i) {
  if (i) result += ", ";
  result += values[i].dump();
 }
 return result.empty() ? "none" : result;
}
}  // namespace
std::size_t physicalLines(std::string_view source) { return static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n')) + (!source.empty() && source.back() != '\n' ? 1 : 0); }
Json buildReview(const Json& files, const Json& rawReport) {
 std::map<std::string, std::vector<const Json*>> classified;
 for (const auto& occurrence : rawReport.at("classified_context")) classified[stringAt(occurrence, "path")].push_back(&occurrence);
 struct Group {
  std::string key;
  std::vector<Entry> entries;
  std::map<std::string, std::size_t> positions;
 };
 std::vector<Group> groups;
 std::map<std::string, std::size_t> groupPositions;
 for (const auto& file : files) {
  if (file.at("language") != "cpp") continue;
  const auto& path = stringAt(file, "path");
  auto entries = structuralOccurrences(path, stringAt(file, "source"));
  for (const auto* candidate : classified[path]) {
   const auto& categories = candidate->at("categories");
   const auto includes = [&](std::string_view category) { return std::any_of(categories.begin(), categories.end(), [&](const Json& value) { return value == category; }); };
   const std::string kind = includes("descriptor_exclusion") ? "cli_exclusion" : includes("nested_descriptor") ? "cli_binding" : includes("qualified_name") ? "qualification" : "";
   if (!kind.empty() && (kind == "qualification" || candidate->value("complete", false))) {
    auto occurrence = *candidate;
    const auto owner = occurrence.at("context").at("owner");
    occurrence.erase("context");
    occurrence["owner"] = owner;
    entries.push_back(shapedOccurrence(kind, std::move(occurrence)));
   }
  }
  for (auto& entry : entries) {
   const auto [position, inserted] = groupPositions.try_emplace(entry.key, groups.size());
   if (inserted) groups.push_back({entry.key, {}, {}});
   auto& group = groups[position->second];
   const auto [slot, fresh] = group.positions.try_emplace(occurrenceKey(entry.occurrence), group.entries.size());
   if (fresh)
    group.entries.push_back(std::move(entry));
   else
    group.entries[slot->second] = std::move(entry);
  }
 }
 std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return locale_compare(a.key, b.key, true) < 0; });
 Json families = Json::array();
 for (auto& group : groups) {
  auto& entries = group.entries;
  if (entries.size() < 3) continue;
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
   const auto order = locale_compare(stringAt(a.occurrence, "path"), stringAt(b.occurrence, "path"), true);
   return order ? order < 0 : a.occurrence.at("start_offset") < b.occurrence.at("start_offset");
  });
  const auto& first = entries.front();
  std::vector<std::size_t> varying;
  Json invariants = Json::array(), slots = Json::array(), occurrences = Json::array();
  for (std::size_t i = 0; i < first.values.size(); ++i) {
   const bool differs = std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.values[i] != first.values[i]; });
   if (differs) {
    varying.push_back(i);
    invariants.push_back(nullptr);
    slots.push_back({{"token", i}, {"kind", first.shape[i]}});
   } else
    invariants.push_back(first.values[i]);
  }
  std::set<std::string> paths, owners;
  std::size_t unresolved = 0;
  std::vector<const Json*> spans;
  for (const auto& entry : entries) {
   const auto& occurrence = entry.occurrence;
   const auto& path = stringAt(occurrence, "path");
   paths.insert(path);
   if (occurrence.at("owner").is_null())
    ++unresolved;
   else
    owners.insert(path + ':' + stringAt(occurrence, "owner"));
   spans.push_back(&occurrence);
   auto result = occurrence;
   result["varying_values"] = Json::array();
   for (const auto slot : varying) result["varying_values"].push_back(entry.values[slot]);
   occurrences.push_back(std::move(result));
  }
  families.push_back(
   {{"id", families.size() + 1}, {"status", "unreviewed"}, {"kind", first.kind}, {"invariant_tokens", std::move(invariants)}, {"varying_slots", std::move(slots)}, {"occurrence_count", entries.size()},
    {"file_count", paths.size()}, {"owner_count", owners.size()}, {"unresolved_owner_occurrences", unresolved}, {"covered_lines", unionLines(spans)}, {"occurrences", std::move(occurrences)}});
 }
 std::vector<Json*> occurrences;
 std::vector<const Json*> spans;
 std::set<std::string> distinct;
 std::map<std::string, std::set<std::size_t>> familyIds;
 for (auto& family : families) {
  for (auto& occurrence : family.at("occurrences")) {
   occurrences.push_back(&occurrence);
   spans.push_back(&occurrence);
   distinct.insert(occurrenceKey(occurrence));
   familyIds[stringAt(occurrence, "path")].insert(family.at("id").get<std::size_t>());
  }
 }
 associateRawMatches(occurrences, rawReport.at("raw_matches"));
 Json inventory = Json::array(), queue = Json::array();
 std::size_t large = 0;
 for (const auto& file : files) {
  const auto& path = stringAt(file, "path");
  const auto& source = stringAt(file, "source");
  const auto& language = stringAt(file, "language");
  Json item{{"path", path}, {"language", language}};
  if (file.contains("sha256")) item["sha256"] = file.at("sha256");
  const auto lineCount = physicalLines(source);
  item["physical_lines"] = lineCount;
  item["bytes"] = source.size();
  item["test_evidence"] = testEvidence(path, source, language);
  item["family_ids"] = familyIds[path];
  item["status"] = "unreviewed";
  item["analysis"] = language == "cpp" ? "bounded lexical patterns" : "size triage only";
  if (lineCount > 500) ++large;
  inventory.push_back(std::move(item));
 }
 std::stable_sort(inventory.begin(), inventory.end(), [](const Json& a, const Json& b) { return locale_compare(stringAt(a, "path"), stringAt(b, "path"), true) < 0; });
 for (const auto& file : inventory) {
  if (file.at("physical_lines").get<std::size_t>() > 500 || !file.at("family_ids").empty()) queue.push_back(file.at("path"));
 }
 Json summary{{"authored_files", inventory.size()}, {"large_files", large}, {"families", families.size()}, {"distinct_occurrences", distinct.size()}, {"covered_lines", unionLines(spans)}};
 return {
  {"purpose", "Manual lexical review candidates; no semantic-equivalence or removable-line claim"}, {"threshold", 500}, {"inventory", std::move(inventory)}, {"queue", std::move(queue)},
  {"families", std::move(families)}, {"summary", std::move(summary)}
 };
}
std::string renderReviewMarkdown(const Json& review) {
 const auto queued = review.at("queue").get<std::set<std::string>>();
 std::string result = "# Declaration and large-file review\n\n" + stringAt(review, "purpose") +
                      ".\n\n"
                      "All entries are **unreviewed**. Covered lines are the union of evidence spans, not removable lines or production-line counts.\n\n"
                      "| File | Language | Physical lines | Test evidence | Families |\n| --- | --- | ---: | --- | ---: |\n";
 for (const auto& file : review.at("inventory")) {
  if (!queued.contains(stringAt(file, "path"))) continue;
  std::string tests;
  for (auto it = file.at("test_evidence").begin(); it != file.at("test_evidence").end(); ++it) {
   if (!it.value().get<bool>()) continue;
   if (!tests.empty()) tests += ", ";
   tests += it.key();
  }
  if (tests.empty()) tests = "none recognized";
  result += "| " + markdownEscape(stringAt(file, "path"), false) + " | " + stringAt(file, "language") + " | " + file.at("physical_lines").dump() + " | " + tests + " | " +
            std::to_string(file.at("family_ids").size()) + " |\n";
 }
 for (const auto& family : review.at("families")) {
  result += "\n## Family " + family.at("id").dump() + ": " + stringAt(family, "kind") + " (unreviewed)\n\n" + family.at("occurrence_count").dump() + " distinct occurrences in " +
            family.at("file_count").dump() + " files; " + family.at("covered_lines").dump() + " evidence lines after overlap merging.\n\nVarying token slots: ";
  std::string slots;
  for (const auto& slot : family.at("varying_slots")) {
   if (!slots.empty()) slots += ", ";
   slots += slot.at("token").dump() + " (" + stringAt(slot, "kind") + ')';
  }
  result += (slots.empty() ? "none" : slots) + ". Full values and anchors are in JSON.\n";
  const auto& occurrences = family.at("occurrences");
  for (std::size_t i = 0; i < std::min<std::size_t>(3, occurrences.size()); ++i) {
   const auto& occurrence = occurrences[i];
   const auto& rawIds = occurrence.at("raw_match_ids");
   result += "\n- " + markdownEscape(stringAt(occurrence, "path"), false) + ':' + occurrence.at("start").dump() + ':' + occurrence.at("column").dump() + "–" + occurrence.at("end").dump() + ':' +
             occurrence.at("endColumn").dump() + "; raw match IDs: " + joinedNumbers(rawIds, 12) + (rawIds.size() > 12 ? " (more in JSON)" : "") + "\n\n<pre>";
   const auto text = cpd::toUtf16(stringAt(occurrence, "text"));
   auto end = std::min<std::size_t>(800, text.size());
   std::size_t lines = 0;
   for (std::size_t at = 0; at < end; ++at) {
    if (text[at] == u'\n' && ++lines == 8) {
     end = at;
     break;
    }
   }
   auto excerpt = std::u16string(text.begin(), text.begin() + end);
   // Node's UTF-8 file writer replaces a surrogate split by slice(0, 800).
   if (!excerpt.empty() && excerpt.back() >= 0xd800 && excerpt.back() <= 0xdbff) excerpt.back() = u'\ufffd';
   result += markdownEscape(cpd::fromUtf16(excerpt), true);
   if (end < text.size()) result += "\n… (bounded excerpt)";
   result += "</pre>\n";
  }
  if (occurrences.size() > 3) result += "\nRemaining occurrence anchors and varying values are in JSON.\n";
 }
 return result;
}
}  // namespace mmltk::tools::review
