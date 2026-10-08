module;
// Declaration evidence and syntactic authoring rules. Uncertain C++ lookup or
// preprocessing remains an explicit manual case, never rewrite authority.

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

export module mmltk.tools.cleanup.declaration_patterns;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cpp_text;

export {
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

}

namespace mmltk::tools::declarations {

const std::map<std::string, std::string> ANNOTATION_MACROS = {
    {"MaxBytes", "MMLTK_MAX_BYTES"}, {"MinBytes", "MMLTK_MIN_BYTES"},
    {"MaxItems", "MMLTK_MAX_ITEMS"}, {"Minimum", "MMLTK_MINIMUM"},
    {"Maximum", "MMLTK_MAXIMUM"}, {"Finite", "MMLTK_FINITE"},
    {"RuntimeDestination", "MMLTK_RUNTIME_DESTINATION"},
    {"Presentation", "MMLTK_PRESENTATION"}, {"CatalogProvider", "MMLTK_CATALOG"}};
const std::set<std::string> MACRO_NAMES = [] {
  std::set<std::string> names;
  for (const auto& [policy, macro] : ANNOTATION_MACROS) names.insert(macro);
  names.insert({"MMLTK_MAX_PATH_BYTES", "MMLTK_MAX_NAME_BYTES", "MMLTK_MINIMUM_VALUE"});
  return names;
}();

namespace {

constexpr std::string_view policyNamespace = "mmltk::frameworks::reflection";
using Tokens = std::span<const cpd::Token>;

using text::oneOf;

using text::word;

bool identifier(std::string_view text) {
  if (text.empty() || !((text.front() >= 'a' && text.front() <= 'z') ||
                        (text.front() >= 'A' && text.front() <= 'Z') || text.front() == '_'))
    return false;
  return std::all_of(text.begin(), text.end(), [](unsigned char value) { return word(value); });
}

using mmltk::tools::text::whitespace;

std::string slice(std::u16string_view source, std::size_t start, std::size_t end) {
  start = std::min(start, source.size());
  end = std::clamp(end, start, source.size());
  return cpd::fromUtf16(source.substr(start, end - start));
}

std::string trim(std::u16string_view source) {
  return cpd::fromUtf16(text::trimUnits(source));
}

std::size_t lineAt(const std::vector<std::size_t>& lines, std::size_t offset) {
  return static_cast<std::size_t>(std::upper_bound(lines.begin(), lines.end(), offset) - lines.begin());
}

std::string_view tokenText(Tokens tokens, std::size_t position) {
  return position < tokens.size() ? std::string_view(tokens[position].text) : std::string_view{};
}

std::string spelling(Tokens tokens) {
  std::string result;
  for (const auto& token : tokens) result += token.text;
  return result;
}

struct Region {
  std::size_t start = 0;
  std::size_t end = 0;
  std::string text;
  std::string directive;
};

struct Preprocessing {
  std::vector<Region> regions;
  std::vector<Region> conditional;
};

bool inside(const std::vector<Region>& regions, std::size_t offset) {
  return std::any_of(regions.begin(), regions.end(), [offset](const auto& region) {
    return region.start <= offset && offset < region.end;
  });
}

std::string directiveWord(std::u16string_view text, std::size_t& cursor) {
  while (cursor < text.size() && whitespace(text[cursor])) ++cursor;
  const auto start = cursor;
  while (cursor < text.size() && word(text[cursor])) ++cursor;
  return slice(text, start, cursor);
}

Preprocessing preprocessing(std::u16string_view source, Tokens tokens) {
  Preprocessing result;
  std::vector<std::size_t> stack;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    const auto priorNewline = token.offset == 0 ? std::u16string_view::npos : source.rfind(u'\n', token.offset - 1);
    const auto startOfLine = priorNewline == std::u16string_view::npos ? 0 : priorNewline + 1;
    if (token.text != "#" || (index == 0 ? 0 : tokens[index - 1].end) > startOfLine ||
        (!result.regions.empty() && token.offset < result.regions.back().end)) continue;
    auto end = source.find(u'\n', token.end);
    if (end == std::u16string_view::npos) end = source.size();
    while (end < source.size()) {
      auto tail = end;
      if (tail > token.offset && source[tail - 1] == u'\r') --tail;
      if (tail <= token.offset || source[tail - 1] != u'\\') break;
      end = source.find(u'\n', end + 1);
      if (end == std::u16string_view::npos) end = source.size();
    }
    const auto text = source.substr(token.offset, end - token.offset);
    std::size_t cursor = 1;
    const auto directive = directiveWord(text, cursor);
    result.regions.push_back({token.offset, end, cpd::fromUtf16(text), directive});
    if (oneOf(directive, {"if", "ifdef", "ifndef"})) stack.push_back(token.offset);
    if (directive == "endif" && !stack.empty()) {
      const auto start = stack.back();
      stack.pop_back();
      if (stack.empty()) result.conditional.push_back({start, end, {}, {}});
    }
  }
  if (!stack.empty()) result.conditional.push_back({stack.front(), source.size(), {}, {}});
  return result;
}

Json preprocessorJson(const Preprocessing& preprocessing) {
  Json regions = Json::array(), conditional = Json::array();
  for (const auto& region : preprocessing.regions) {
    Json item{{"start", region.start}, {"end", region.end}, {"text", region.text}};
    if (!region.directive.empty()) item["directive"] = region.directive;
    regions.push_back(std::move(item));
  }
  for (const auto& region : preprocessing.conditional)
    conditional.push_back({{"start", region.start}, {"end", region.end}});
  return {{"regions", std::move(regions)}, {"conditional", std::move(conditional)}};
}

std::vector<std::string> conflicts(std::string_view path, const Preprocessing& preprocessing) {
  std::vector<std::string> result;
  if (path == ANNOTATION_HEADER) return result;
  for (const auto& region : preprocessing.regions) {
    const auto text = cpd::toUtf16(region.text);
    std::size_t cursor = 1;
    const auto directive = directiveWord(text, cursor);
    if (!oneOf(directive, {"define", "undef"}) || cursor == text.size() || !whitespace(text[cursor])) continue;
    const auto name = directiveWord(text, cursor);
    if (MACRO_NAMES.contains(name)) result.push_back(name);
  }
  return result;
}

struct Alias {
  std::string target;
  bool global = false;
  std::size_t start = 0;
  std::size_t end = std::numeric_limits<std::size_t>::max();
};
using Aliases = std::map<std::string, std::vector<Alias>>;

Aliases policyAliases(const cpd::SourceIndex& index, const Preprocessing& preprocessing) {
  const Tokens tokens = index.tokens;
  Aliases aliases;
  std::vector<std::size_t> scopes;
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    while (!scopes.empty() && scopes.back() < i) scopes.pop_back();
    const auto pair = index.pairs.find(i);
    if (tokens[i].text == "{" && pair != index.pairs.end() && pair->second > i) scopes.push_back(pair->second);
    if (inside(preprocessing.regions, tokens[i].offset) || inside(preprocessing.conditional, tokens[i].offset) ||
        tokens[i].text != "namespace" || tokenText(tokens, i + 2) != "=") continue;
    auto end = i + 3;
    while (end < tokens.size() && tokens[end].text != ";") ++end;
    if (end == tokens.size()) continue;
    aliases[tokens[i + 1].text].push_back({spelling(tokens.subspan(i + 3, end - i - 3)), scopes.empty(),
        tokens[end].end, scopes.empty() ? std::numeric_limits<std::size_t>::max() : tokens[scopes.back()].offset});
  }
  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    if (oneOf(tokens[i].text, {"class", "struct", "using", "typename"}) ||
        (tokens[i].text == "namespace" && tokenText(tokens, i + 2) != "=")) {
      if (auto found = aliases.find(tokens[i + 1].text); found != aliases.end()) found->second.clear();
    }
  }
  return aliases;
}

std::string canonicalName(Tokens tokens, const Aliases& aliases) {
  const auto qualified = spelling(tokens);
  if (!qualified.starts_with("::")) return {};
  const auto name = qualified.substr(2);
  if (name.starts_with(std::string(policyNamespace) + "::")) return name;
  const auto split = name.find("::");
  if (split == std::string::npos || split == 0 || tokens.empty()) return {};
  const auto found = aliases.find(name.substr(0, split));
  if (found == aliases.end()) return {};
  const Alias* visible = nullptr;
  for (const auto& alias : found->second)
    if (alias.global && alias.start <= tokens.front().offset && tokens.front().offset < alias.end) visible = &alias;
  if (visible == nullptr || visible->target != "::" + std::string(policyNamespace)) return {};
  return std::string(policyNamespace) + name.substr(split);
}

struct Rewrite {
  std::string reason;
  std::string replacement;
  std::string policy;
};

Rewrite annotationRewrite(Tokens tokens, std::u16string_view source, const Aliases& aliases) {
  const auto braceIt = std::find_if(tokens.begin(), tokens.end(), [](const auto& token) { return token.text == "{"; });
  if (braceIt == tokens.end() || tokens.back().text != "}")
    return {"annotation expression is not a known policy construction", {}, {}};
  const auto brace = static_cast<std::size_t>(braceIt - tokens.begin());
  int depth = 0;
  auto close = brace;
  for (; close < tokens.size(); ++close) {
    if (tokens[close].text == "{") ++depth;
    if (tokens[close].text == "}" && --depth == 0) break;
  }
  if (close != tokens.size() - 1) return {"annotation contains unfamiliar expressions or composition", {}, {}};
  const auto head = tokens.first(brace);
  const auto values = tokens.subspan(brace + 1, tokens.size() - brace - 2);
  const auto angleIt = std::find_if(head.begin(), head.end(), [](const auto& token) { return token.text == "<"; });
  const auto angle = static_cast<std::size_t>(angleIt - head.begin());
  const bool templated = angle < head.size();
  const auto name = canonicalName(head.first(angle), aliases);
  const auto prefix = std::string(policyNamespace) + "::";
  const auto policy = name.starts_with(prefix) ? name.substr(prefix.size()) : std::string{};
  const auto macroIt = ANNOTATION_MACROS.find(policy);
  if (macroIt == ANNOTATION_MACROS.end())
    return {"policy lookup is not proven global or is outside the annotation spelling surface", {}, {}};
  const auto& macro = macroIt->second;
  const auto value = values.empty() ? std::string{} : slice(source, values.front().offset, values.back().end);
  const auto typeTokens = templated && angle + 1 < head.size() ? head.subspan(angle + 1, head.size() - angle - 2) : Tokens{};
  const auto type = templated && head.back().text.ends_with('>')
      ? trim(source.substr(head[angle].end, head.back().end - 1 - head[angle].end)) : std::string{};
  if (templated && !oneOf(head.back().text, {">", ">>"}))
    return {"complex template closure requires manual review", {}, {}};
  if (oneOf(policy, {"Minimum", "Maximum"})) {
    if (type.empty() && !value.empty() && policy == "Minimum") return {{}, "MMLTK_MINIMUM_VALUE(" + value + ")", policy};
    if (type.empty() || value.empty() || std::any_of(typeTokens.begin(), typeTokens.end(), [](const auto& token) { return token.text == ","; }))
      return {"typed bound requires one visible type argument; use a scoped type alias for template commas", {}, {}};
    return {{}, macro + "(" + type + ", " + value + ")", policy};
  }
  if (oneOf(policy, {"Presentation", "CatalogProvider"})) {
    if (type.empty() || !value.empty()) return {"unfamiliar templated policy construction", {}, {}};
    return {{}, macro + "(" + type + ")", policy};
  }
  if (!type.empty()) return {"unfamiliar policy template arguments", {}, {}};
  if (oneOf(policy, {"Finite", "RuntimeDestination"})) {
    if (!value.empty()) return {"marker policy has unfamiliar initialization", {}, {}};
    return {{}, macro, policy};
  }
  if (value.empty()) return {"bound policy has no explicit value", {}, {}};
  if (policy == "MaxBytes") {
    const auto bound = canonicalName(values, aliases);
    if (bound == prefix + "kMaximumPathBytes") return {{}, "MMLTK_MAX_PATH_BYTES", policy};
    if (bound == prefix + "kMaximumNameBytes") return {{}, "MMLTK_MAX_NAME_BYTES", policy};
  }
  return {{}, macro + "(" + value + ")", policy};
}

bool modulePath(std::string_view path) { return path.ends_with(".cppm") || path.ends_with(".cppm.in"); }

bool typedLimit(std::string_view policy) {
  return oneOf(policy, {"Minimum", "Maximum", "MaxBytes", "MinBytes", "MaxItems"});
}

Json sourceSpan(std::string_view path, std::u16string_view source,
                const std::vector<std::size_t>& lines, std::size_t start, std::size_t end) {
  const auto startLine = lineAt(lines, start);
  const auto endLine = lineAt(lines, end > start ? end - 1 : start);
  return {{"path", path}, {"start", startLine}, {"end", endLine},
          {"column", start - lines[startLine - 1] + 1}, {"endColumn", end - lines[endLine - 1] + 1},
          {"start_offset", start}, {"end_offset", end}, {"text", slice(source, start, end)}};
}

std::string preprocessingArgument(const Region& region) {
  const auto text = cpd::toUtf16(region.text);
  std::size_t cursor = 1;
  directiveWord(text, cursor);
  while (cursor < text.size() && whitespace(text[cursor])) ++cursor;
  if (cursor >= text.size() || (text[cursor] != u'<' && text[cursor] != u'"')) return {};
  const auto start = ++cursor;
  while (cursor < text.size() && text[cursor] != u'>' && text[cursor] != u'"') ++cursor;
  return cursor < text.size() && cursor > start ? slice(text, start, cursor) : std::string{};
}

bool pragmaOnce(const Region& region) {
  const auto text = cpd::toUtf16(region.text);
  std::size_t cursor = 1;
  if (directiveWord(text, cursor) != "pragma" || cursor >= text.size() || !whitespace(text[cursor])) return false;
  return directiveWord(text, cursor) == "once";
}

}  // namespace

std::vector<std::size_t> lineStarts(std::string_view source) { return text::lineStarts(std::u16string_view(cpd::toUtf16(source))); }

Json sourceOccurrence(std::string_view path, std::string_view source, const Json& occurrence,
                      const std::vector<std::size_t>& lines) {
  Json result = occurrence;
  result["path"] = path;
  const auto text = cpd::toUtf16(source);
  const auto [start, end] = cpd::sourceRange(std::u16string_view(text), result, lines);
  result["start_offset"] = start;
  result["end_offset"] = end;
  result["text"] = slice(text, start, end);
  return result;
}

Json sourceOccurrence(std::string_view path, std::string_view source, const Json& occurrence) {
  return sourceOccurrence(path, source, occurrence, lineStarts(source));
}

Json preprocessorRegions(std::string_view source, const std::vector<cpd::Token>& tokens) {
  return preprocessorJson(preprocessing(cpd::toUtf16(source), tokens));
}

Json preprocessorRegions(std::string_view source) {
  const auto text = cpd::toUtf16(source);
  return preprocessorJson(preprocessing(text, cpd::tokenizeCpp(std::u16string_view(text))));
}

std::vector<std::string> conflictingMacros(std::string_view path, std::string_view source) {
  if (path == ANNOTATION_HEADER) return {};
  const auto text = cpd::toUtf16(source);
  return conflicts(path, preprocessing(text, cpd::tokenizeCpp(std::u16string_view(text))));
}

std::set<std::string> inventoryMacroConflicts(const std::map<std::string, std::string>& sources) {
  std::set<std::string> result;
  for (const auto& [path, source] : sources) {
    const auto names = conflictingMacros(path, source);
    result.insert(names.begin(), names.end());
  }
  return result;
}

namespace {
struct AngleSpan { std::size_t end; int depth; };
AngleSpan scanAngles(const cpd::SourceIndex& index, std::size_t open, std::size_t limit,
                     bool stop_on_underflow = true, bool skip_groups = false) {
  int depth = 1;
  auto end = open + 1;
  for (; end < limit && (stop_on_underflow ? depth > 0 : depth != 0); ++end) {
    if (skip_groups) {
      const auto close = index.pairs.find(end);
      if (close != index.pairs.end() && close->second > end) { end = close->second; continue; }
    }
    if (index.tokens[end].text == "<") ++depth;
    else if (oneOf(index.tokens[end].text, {">", ">>"}))
      depth -= static_cast<int>(index.tokens[end].text.size());
  }
  return {end, depth};
}
} // namespace

std::optional<std::size_t> invocationEnd(const cpd::SourceIndex& index, std::size_t start) {
  const Tokens tokens = index.tokens;
  if (start >= tokens.size()) return std::nullopt;
  const auto limit = start + std::min<std::size_t>(tokens.size() - start, 2048);
  auto next = start + 1;
  if (tokenText(tokens, next) == "<") {
    const auto span = scanAngles(index, next, limit, true, true);
    if (span.depth != 0) return std::nullopt;
    next = span.end;
  }
  if (tokenText(tokens, next) != "(") return std::nullopt;
  const auto close = index.pairs.find(next);
  if (close != index.pairs.end() && close->second > next && close->second < limit) return close->second + 1;
  return std::nullopt;
}

Json classifyDeclarations(std::string_view path, std::string_view source, const Options& options) {
  const cpd::SourceIndex index(source);
  const auto textSource = cpd::toUtf16(source);
  const Tokens tokens = index.tokens;
  const auto& lines = index.lines;
  const auto preprocessingInfo = preprocessing(textSource, tokens);
  const auto aliases = index.balanced ? policyAliases(index, preprocessingInfo) : Aliases{};
  auto localConflicts = options.macroConflicts;
  const auto local = conflicts(path, preprocessingInfo);
  localConflicts.insert(local.begin(), local.end());
  Json candidates = Json::array();
  struct Annotation { std::size_t start; std::size_t end; std::size_t candidate; };
  struct Scope { std::size_t end; Json name; };
  std::vector<Annotation> annotations;
  std::vector<Scope> scopes;
  std::vector<Json> declarationOwners;
  declarationOwners.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    while (!scopes.empty() && scopes.back().end <= i) scopes.pop_back();
    const auto pair = index.pairs.find(i);
    if (tokens[i].text == "{" && pair != index.pairs.end() && pair->second > i) {
      std::string_view name;
      if (i > 0) {
        const auto previous = tokens[i - 1].text == "=" ? (i >= 2 ? i - 2 : tokens.size()) : i - 1;
        name = tokenText(tokens, previous);
      }
      scopes.push_back({pair->second, identifier(name) ? Json(name) : scopes.empty() ? Json(nullptr) : scopes.back().name});
    }
    declarationOwners.push_back(scopes.empty() ? Json(nullptr) : scopes.back().name);
  }
  const auto tokenEnd = [&](std::size_t position) {
    const auto pair = index.pairs.find(position);
    return pair != index.pairs.end() && pair->second > position ? pair->second : position;
  };
  const auto context = [&](std::size_t start, std::size_t end) -> Json {
    auto finish = end;
    while (finish < tokens.size() && finish - end < 150 && !oneOf(tokens[finish].text, {";", "}"})) ++finish;
    auto declarationEnd = end;
    while (declarationEnd < finish && !oneOf(tokens[declarationEnd].text, {"=", "{"})) ++declarationEnd;
    const auto member = declarationEnd > end ? tokenText(tokens, declarationEnd - 1) : std::string_view{};
    const auto startLine = std::max<std::size_t>(1, lineAt(lines, tokens[start].offset) - 1);
    const auto endLine = std::min(lines.size(), lineAt(lines, tokens[std::max(start, end - 1)].end) + 1);
    Json owner = declarationOwners[start];
    if (index.recordOwners[start] >= 0) owner = index.records[static_cast<std::size_t>(index.recordOwners[start])].name;
    else if (index.owners[start] >= 0) owner = index.functions[static_cast<std::size_t>(index.owners[start])].name;
    return {{"owner", owner}, {"member", identifier(member) ? Json(member) : Json(nullptr)},
            {"start", startLine}, {"end", endLine},
            {"text", slice(textSource, lines[startLine - 1], endLine < lines.size() ? lines[endLine] : textSource.size())}};
  };
  const auto add = [&](std::size_t start, std::size_t end, Json categories,
                       std::string_view disposition, std::string_view reason, const Json& extra = Json::object()) {
    if (end <= start) return candidates.size();
    auto candidate = sourceSpan(path, textSource, lines, tokens[start].offset, tokens[end - 1].end);
    candidate["categories"] = std::move(categories);
    candidate["context"] = context(start, end);
    candidate["disposition"] = disposition;
    candidate["reason"] = reason;
    candidate.update(extra);
    const auto position = candidates.size();
    candidates.push_back(std::move(candidate));
    return position;
  };
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& text = tokens[i].text;
    if (tokens[i].kind != "code" || inside(preprocessingInfo.regions, tokens[i].offset)) continue;
    if (text == "[" && tokenText(tokens, i + 1) == "[" && tokenText(tokens, i + 2) == "=") {
      const auto pair = index.pairs.find(i);
      if (pair == index.pairs.end() || pair->second <= i) {
        add(i, std::min(i + 4, tokens.size()), {"reflection_annotation"}, "manual", "unbalanced annotation syntax");
        continue;
      }
      const auto close = pair->second;
      // Balanced [[...]] guarantees two closing brackets; a malformed but paired
      // input still passes an empty expression to the conservative classifier.
      const auto result = annotationRewrite(close >= i + 4 ? tokens.subspan(i + 3, close - i - 4) : Tokens{}, textSource, aliases);
      const auto annotationTokens = cpd::tokenizeCpp(std::u16string_view(textSource).substr(tokens[i].offset, tokens[close].end - tokens[i].offset), true);
      const bool hasComment = std::any_of(annotationTokens.begin(), annotationTokens.end(), [](const auto& token) { return token.kind == "comment"; });
      const auto macroEnd = result.replacement.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
      const bool conflict = !result.replacement.empty() && localConflicts.contains(result.replacement.substr(0, macroEnd));
      const auto reason = modulePath(path) ? std::string("module authoring requires manual dependency placement") :
          inside(preprocessingInfo.conditional, tokens[i].offset) ? std::string("conditional preprocessing requires manual lookup") :
          hasComment ? std::string("comment inside annotation must retain its source attachment") :
          conflict ? std::string("conflicting annotation macro definition or undefinition") : result.reason;
      Json categories = {"reflection_annotation"};
      if (typedLimit(result.policy)) categories.push_back("typed_limit");
      Json extra = Json::object();
      if (!result.policy.empty()) extra["policy"] = result.policy;
      if (reason.empty()) extra["replacement"] = result.replacement;
      const auto candidate = add(i, close + 1, std::move(categories), reason.empty() ? "rewrite" : "manual",
          reason.empty() ? "canonical typed construction; macro expands to the same annotation expression" : reason, extra);
      annotations.push_back({i, close + 1, candidate});
      i = close;
      continue;
    }
    if (text == "template" && tokenText(tokens, i + 1) == "<") {
      const auto end = scanAngles(index, i + 1, tokens.size(), false).end;
      add(i, end, {"template_declaration"}, "manual", "template constraints and specialization ownership need semantic review");
    }
    if (MACRO_NAMES.contains(text)) {
      const auto end = tokenText(tokens, i + 1) == "(" ? tokenEnd(i + 1) + 1 : i + 1;
      auto policy = text == "MMLTK_MINIMUM_VALUE" ? std::string("Minimum") : std::string("MaxBytes");
      for (const auto& [name, macro] : ANNOTATION_MACROS) if (macro == text) { policy = name; break; }
      Json categories = {"reflection_annotation", "shortened_annotation"};
      if (typedLimit(policy)) categories.push_back("typed_limit");
      const std::string_view reason = localConflicts.contains(text) ? "conflicting annotation macro definition or undefinition" :
          modulePath(path) ? "module authoring requires manual dependency placement" :
          inside(preprocessingInfo.conditional, tokens[i].offset) ? "conditional preprocessing requires manual lookup" : "";
      const auto candidate = add(i, end, std::move(categories), reason.empty() ? "retained" : "manual",
          reason.empty() ? "already uses the canonical syntax-only spelling" : reason, {{"policy", policy}});
      annotations.push_back({i, end, candidate});
      i = end - 1;
      continue;
    }
    if (oneOf(text, {"MMLTK_REFLECT_FIELDS", "MMLTK_REFLECT_ENUM"})) {
      const auto end = tokenText(tokens, i + 1) == "(" ? tokenEnd(i + 1) + 1 : i + 1;
      add(i, end, {"registration_macro"}, "retained", "canonical declaration-local ADL materialization; shortening would hide the boundary");
      i = end - 1;
      continue;
    }
    if (text == "operator" && tokenText(tokens, i + 1) == "==") {
      auto end = i + 2;
      bool defaulted = false;
      while (end < tokens.size() && !oneOf(tokens[end].text, {";", "{"})) {
        defaulted = defaulted || tokens[end].text == "default";
        ++end;
      }
      if (defaulted) add(i, std::min(end + 1, tokens.size()), {"defaulted_equality"}, "retained",
          "ordinary equality keeps qualifiers, visibility and diagnostics explicit");
      continue;
    }
    if (text == "[" && tokenText(tokens, i + 1) == "[") {
      auto end = tokenEnd(i) + 1;
      if (tokenText(tokens, end) == "__device__" && tokenText(tokens, end + 1) == "__forceinline__") end += 2;
      add(i, end, {"attribute_qualifier"}, "manual", "standard attributes remain explicit; repeated CUDA qualifier groups can use a local valid macro");
      i = end - 1;
      continue;
    }
    if (oneOf(text, {"static_cast", "reinterpret_cast"})) {
      auto end = i + 1;
      while (end < tokens.size() && end < i + 40 && tokens[end].text != "(") ++end;
      if (tokenText(tokens, end) == "(") add(i, tokenEnd(end) + 1, {"cast"}, "manual", "conversion semantics, evaluation and alignment need an owner-level decision");
      continue;
    }
    if (text == "std" && tokenText(tokens, i + 1) == "::" && oneOf(tokenText(tokens, i + 2), {"optional", "vector", "array", "span", "inplace_vector"})) {
      auto end = i + 3;
      if (tokenText(tokens, end) == "<") {
        end = scanAngles(index, end, tokens.size()).end;
      }
      Json categories = {tokens[i + 2].text == "optional" ? "optional_declaration" : "collection_declaration"};
      if (end - i > 12) categories.push_back("long_type");
      add(i, end, std::move(categories), "retained", "standard vocabulary preserves ownership and optionality without a parallel alias system");
      continue;
    }
    if (identifier(text) && tokenText(tokens, i + 1) == "::" && (i == 0 || tokens[i - 1].text != "::")) {
      auto end = i + 1;
      std::size_t depth = 0;
      while (tokenText(tokens, end) == "::" && identifier(tokenText(tokens, end + 1))) { end += 2; ++depth; }
      if (depth >= 2) add(i, end, {"qualified_name"}, "manual", "use a meaningful scoped using declaration only after checking owner and lookup");
    }
    if ((oneOf(text, {"option", "custom_option", "negative_flag", "option_with_item_policy", "unexposed"}) && tokenText(tokens, i + 1) == "<") ||
        oneOf(text, {"MMLTK_CLI_OPTION", "MMLTK_CLI_NAMED"})) {
      const auto end = invocationEnd(index, i);
      add(i, end.value_or(i + 1), {text == "unexposed" ? "descriptor_exclusion" : "nested_descriptor"}, "manual",
          end ? "complete lexical binding; preserve every option identity and policy" : "incomplete or over-limit binding requires manual inspection",
          {{"complete", end.has_value()}});
    }
  }
  for (std::size_t i = 0; i < annotations.size();) {
    auto end = i + 1;
    while (end < annotations.size() && annotations[end - 1].end == annotations[end].start) ++end;
    if (end - i > 1) {
      bool unknown = false;
      for (auto j = i; j < end; ++j) unknown = unknown || candidates[annotations[j].candidate]["disposition"] == "manual";
      for (auto j = i; j < end; ++j) {
        auto& candidate = candidates[annotations[j].candidate];
        candidate["categories"].push_back("adjacent_annotation_composition");
        if (unknown && candidate["disposition"] == "rewrite") {
          candidate["disposition"] = "manual";
          candidate["reason"] = "adjacent composition contains an unfamiliar or ambiguous annotation";
          candidate.erase("replacement");
        }
      }
    }
    i = end;
  }
  for (const auto& fn : index.functions)
    if (fn.open > fn.start && !inside(preprocessingInfo.regions, tokens[fn.start].offset))
      add(fn.start, fn.open, {"function_signature"}, "retained", "ordinary function signature retains access, qualifiers and exception behavior");
  std::stable_sort(candidates.begin(), candidates.end(), [](const Json& a, const Json& b) {
    return a["start_offset"] != b["start_offset"] ? a["start_offset"] < b["start_offset"] : a["end_offset"] < b["end_offset"];
  });
  return {{"candidates", std::move(candidates)}};
}

Json formatDeclarationSource(std::string_view path, std::string_view source, const Options& options) {
  auto result = classifyDeclarations(path, source, options);
  const auto text = cpd::toUtf16(source);
  Json edits = Json::array();
  bool usesAnnotationMacros = false;
  for (const auto& candidate : result["candidates"]) {
    if (candidate["disposition"] == "rewrite") {
      const auto start = candidate["start_offset"].get<std::size_t>();
      const auto end = candidate["end_offset"].get<std::size_t>();
      auto replacement = candidate["replacement"].get<std::string>();
      if (end < text.size() && !whitespace(text[end])) replacement += ' ';
      edits.push_back({{"start", start}, {"end", end}, {"replacement", replacement}, {"reason", candidate["reason"]}});
    }
    if (candidate["disposition"] == "retained") {
      const auto& categories = candidate["categories"];
      usesAnnotationMacros = usesAnnotationMacros || std::find(categories.begin(), categories.end(), Json("shortened_annotation")) != categories.end();
    }
  }
  const auto preprocessingInfo = preprocessing(text, cpd::tokenizeCpp(std::u16string_view(text)));
  bool hasInclude = false;
  const Region* firstInclude = nullptr;
  const Region* pragma = nullptr;
  for (const auto& region : preprocessingInfo.regions) {
    if (region.directive == "include" && !inside(preprocessingInfo.conditional, region.start)) {
      if (firstInclude == nullptr) firstInclude = &region;
      const auto included = preprocessingArgument(region);
      hasInclude = hasInclude || included == ANNOTATION_HEADER || included == DECLARATION_SURFACE;
    }
    if (pragma == nullptr && pragmaOnce(region)) pragma = &region;
  }
  if ((!edits.empty() || usesAnnotationMacros) && !hasInclude) {
    const auto offset = firstInclude != nullptr ? firstInclude->start : pragma != nullptr ? std::min(text.size(), pragma->end + 1) : 0;
    const auto newline = source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    edits.push_back({{"start", offset}, {"end", offset},
        {"replacement", "#include \"" + std::string(ANNOTATION_HEADER) + "\"" + newline},
        {"reason", "direct self-contained annotation dependency"}});
  }
  std::stable_sort(edits.begin(), edits.end(), [](const Json& a, const Json& b) {
    return a["start"] != b["start"] ? a["start"] < b["start"] : a["end"] < b["end"];
  });
  std::string output;
  output.reserve(source.size());
  std::size_t cursor = 0;
  for (const auto& edit : edits) {
    const auto start = edit["start"].get<std::size_t>();
    if (start < cursor) throw std::runtime_error("overlapping declaration edits in " + std::string(path));
    output += slice(text, cursor, start);
    output += edit["replacement"].get<std::string>();
    cursor = edit["end"].get<std::size_t>();
  }
  output += slice(text, cursor, text.size());
  result["edits"] = std::move(edits);
  result["output"] = output;
  result["changed"] = output != source;
  return result;
}

}  // namespace mmltk::tools::declarations
