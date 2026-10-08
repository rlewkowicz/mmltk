module;
// CPD candidate triage restores names and complete operations; it is not a
// parser or a proof of semantic equivalence.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
export module mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
export {
 namespace mmltk::tools::cpd {
 using Json = nlohmann::ordered_json;
 // Text crosses the public boundary as UTF-8. All source offsets and columns,
 // including token offsets and line starts, count UTF-16 code units like PMD.
 // The JavaScript lexer's single-code-unit fallback can split a surrogate pair:
 // isolated surrogate token text uses lossless WTF-8 internally. Pattern keys
 // escape those units as JSON \uXXXX, so report strings remain valid UTF-8.
 std::u16string toUtf16(std::string_view source);
 std::string fromUtf16(std::u16string_view source);
 std::string jsTrim(std::string_view source);
 struct Token {
  std::string text;
  std::size_t offset = 0;
  std::size_t end = 0;
  std::string kind;
 };
 std::vector<Token> tokenizeCpp(std::string_view source, bool trivia = false);
 std::vector<Token> tokenizeCpp(std::u16string_view source, bool trivia = false);
 std::vector<Token> sourceComments(std::string_view path, std::string_view source);
 std::pair<std::size_t, std::size_t> sourceRange(std::string_view source, const Json& occurrence, const std::vector<std::size_t>& lines);
 std::pair<std::size_t, std::size_t> sourceRange(std::u16string_view source, const Json& occurrence, const std::vector<std::size_t>& lines);
 struct Record {
  std::string name;
  std::size_t start = 0;
  std::size_t open = 0;
  std::size_t end = 0;
  std::optional<std::string> key;
 };
 struct Function {
  std::string name;
  std::size_t start = 0;
  std::size_t open = 0;
  std::size_t end = 0;
  std::size_t firstStatementEnd = 0;
  std::vector<std::vector<Token>> parameters;
  std::set<std::string> locals;
  std::string parameterTypes;
  std::optional<std::size_t> guardEnd;
  std::optional<std::string> prefix;
 };
 struct Parameters {
  std::string name;
  std::size_t open = 0;
  std::size_t close = 0;
 };
 class SourceIndex {
 public:
  explicit SourceIndex(std::string_view source);
  std::string source;
  std::vector<std::size_t> lines;
  std::vector<Token> tokens;
  std::map<std::size_t, std::size_t> pairs;
  bool balanced = true;
  std::vector<Function> functions;
  std::vector<Record> records;
  std::vector<std::ptrdiff_t> owners;
  std::vector<std::ptrdiff_t> recordOwners;
  std::vector<std::size_t> statementStarts;
  std::vector<std::size_t> statementEnds;
  void indexStatements();
  void indexFunctions();
  void indexLocals();
  std::optional<Parameters> findParameters(std::size_t start, std::size_t end) const;
  std::pair<std::size_t, std::size_t> range(const Json& occurrence) const;
  Json occurrence(std::string_view path, std::size_t start, std::size_t end) const;
  bool isBinding(std::size_t start, std::size_t end, const Function& fn) const;
  Json describe(const Json& occurrence);
  std::string recordKey(Record& record);

 private:
  Json describeTokens(const Json& occurrence, std::size_t rawStart, std::size_t rawEnd, std::size_t start, std::size_t end);
  std::u16string sourceUnits_;
 };
 struct FilterOptions {
  // An empty callback selects the native file reader.
  std::function<std::string(const std::string&)> sourceReader;
  bool consolidate = true;
 };
 Json filterCpdCandidates(const Json& candidates, const FilterOptions& options = {});
 }  // namespace mmltk::tools::cpd
}
namespace mmltk::tools::cpd {
namespace {
using text::digit;
using text::letter;
using text::oneOf;
bool word(char16_t ch) { return text::word(ch) || ch == u'$'; }
bool identifier(std::string_view text) {
 return !text.empty() && (letter(static_cast<unsigned char>(text[0])) || text[0] == '_' || text[0] == '$') && std::all_of(text.begin(), text.end(), [](unsigned char ch) { return word(ch); });
}
using mmltk::tools::text::whitespace;
bool highSurrogate(char16_t ch) { return ch >= 0xd800 && ch <= 0xdbff; }
bool lowSurrogate(char16_t ch) { return ch >= 0xdc00 && ch <= 0xdfff; }
bool control(std::string_view text) {
 return oneOf(text, {"if", "for", "while", "switch", "catch", "sizeof", "alignof", "decltype", "noexcept", "requires", "static_assert", "constexpr", "consteval"});
}
bool nonType(std::string_view text) { return control(text) || oneOf(text, {"return", "throw", "case", "co_return", "co_yield", "delete", "new", "break", "continue", "else", "goto", "operator"}); }
bool assertion(std::string_view text) {
 for (const std::string_view name : {"CHECK", "REQUIRE", "ASSERT"}) {
  if (text == name) return true;
  if (!text.starts_with(name) || text.size() <= name.size() + 1 || text[name.size()] != '_') continue;
  if (std::all_of(text.begin() + static_cast<std::ptrdiff_t>(name.size() + 1), text.end(), [](unsigned char ch) { return (ch >= 'A' && ch <= 'Z') || digit(ch) || ch == '_'; })) return true;
 }
 return false;
}
std::string_view at(const std::vector<Token>& tokens, std::size_t index) { return index < tokens.size() ? std::string_view(tokens[index].text) : std::string_view{}; }
std::string_view at(const std::vector<std::string>& tokens, std::size_t index) { return index < tokens.size() ? std::string_view(tokens[index]) : std::string_view{}; }
std::string tokenText(const std::vector<Token>& tokens, std::size_t start, std::size_t end) {
 std::string result;
 for (auto index = start; index < std::min(end, tokens.size()); ++index) {
  if (index != start) result += ' ';
  result += tokens[index].text;
 }
 return result;
}
void appendUtf8(std::string& result, std::uint32_t ch) {
 if (ch < 0x80) {
  result += static_cast<char>(ch);
  return;
 }
 const int continuation = ch < 0x800 ? 1 : ch < 0x10000 ? 2 : 3;
 const unsigned prefix = continuation == 1 ? 0xc0 : continuation == 2 ? 0xe0 : 0xf0;
 result += static_cast<char>(prefix | (ch >> (continuation * 6)));
 for (int shift = (continuation - 1) * 6; shift >= 0; shift -= 6) result += static_cast<char>(0x80 | ((ch >> shift) & 0x3f));
}
std::size_t lineEnd(std::u16string_view source, std::size_t from) {
 const auto end = source.find(u'\n', from);
 return end == std::u16string_view::npos ? source.size() : end;
}
// Like Map, replacement keeps the original insertion position. Group and
// occurrence ordering is part of the report format, so a sorted map is wrong.
template <class Value>
class OrderedMap {
public:
 std::vector<std::pair<std::string, Value>> entries;
 Value* find(const std::string& key) {
  const auto found = positions.find(key);
  return found == positions.end() ? nullptr : &entries[found->second].second;
 }
 Value& get(const std::string& key) {
  if (auto* found = find(key)) return *found;
  positions.emplace(key, entries.size());
  entries.emplace_back(key, Value{});
  return entries.back().second;
 }
 void set(const std::string& key, Value value) { get(key) = std::move(value); }
 std::size_t size() const { return entries.size(); }

private:
 std::unordered_map<std::string, std::size_t> positions;
};
std::size_t quotedEnd(std::u16string_view source, std::size_t quote) {
 const auto delimiter = source[quote];
 for (auto end = quote + 1; end < source.size(); ++end) {
  if (source[end] == delimiter) return end + 1;
  if (source[end] == u'\\') {
   if (++end == source.size()) break;
  }
 }
 return quote;  // No match, allowing the following lexical alternatives.
}
std::size_t stringQuote(std::u16string_view source, std::size_t offset) {
 if (source.substr(offset, 2) == u"u8") return offset + 2;
 if (source[offset] == u'u' || source[offset] == u'U' || source[offset] == u'L') return offset + 1;
 return offset;
}
std::size_t rawStringEnd(std::u16string_view source, std::size_t offset) {
 const auto prefix = stringQuote(source, offset);
 if (source.substr(prefix, 2) != u"R\"") return offset;
 const auto delimiterStart = prefix + 2;
 auto opening = delimiterStart;
 while (opening < source.size() && opening - delimiterStart <= 16 && source[opening] != u'(') {
  const auto ch = source[opening];
  if (ch == u' ' || ch == u')' || ch == u'\\' || ch == u'\t' || ch == u'\r' || ch == u'\n') return offset;
  ++opening;
 }
 if (opening == source.size() || opening - delimiterStart > 16) return offset;
 std::u16string close = u")";
 close.append(source.substr(delimiterStart, opening - delimiterStart));
 close += u'"';
 const auto end = source.find(close, opening + 1);
 return end == std::u16string_view::npos ? offset : end + close.size();
}
std::vector<std::vector<Token>> parameterDeclarations(const std::vector<Token>& tokens, const std::map<std::size_t, std::size_t>& pairs, std::size_t start, std::size_t end) {
 std::vector<std::vector<Token>> result;
 int templateDepth = 0;
 for (auto index = start; index <= end; ++index) {
  if (index != end) {
   const auto& text = tokens[index].text;
   if (text == "<")
    ++templateDepth;
   else if (text == ">" || text == ">>")
    templateDepth = std::max(0, templateDepth - static_cast<int>(text.size()));
   const auto close = pairs.find(index);
   if (close != pairs.end() && close->second > index && close->second < end) {
    index = close->second;
    continue;
   }
   if (text != "," || templateDepth != 0) continue;
  }
  auto finish = start;
  while (finish < index && tokens[finish].text != "=") ++finish;
  result.emplace_back(tokens.begin() + static_cast<std::ptrdiff_t>(start), tokens.begin() + static_cast<std::ptrdiff_t>(finish));
  start = index + 1;
 }
 return result;
}
std::optional<std::string> parameterName(const std::vector<Token>& declaration) {
 if (declaration.empty() || !identifier(declaration.back().text) || at(declaration, declaration.size() - 2) == "::") return std::nullopt;
 const auto count = std::count_if(declaration.begin(), declaration.end(), [](const Token& token) { return identifier(token.text) && token.text != "const"; });
 return count >= 2 ? std::optional(declaration.back().text) : std::nullopt;
}
std::vector<std::string> algorithmReceivers(const std::vector<std::string>& tokens) {
 std::vector<std::string> result;
 std::unordered_map<std::string, std::string> identities;
 const auto identity = [&](const std::string& key) -> const std::string& {
  const auto found = identities.find(key);
  if (found != identities.end()) return found->second;
  const auto name = "$" + std::to_string(identities.size());
  return identities.emplace(key, name).first->second;
 };
 for (std::size_t index = 0; index < tokens.size(); ++index) {
  const auto& text = tokens[index];
  auto end = index;
  if (identifier(text) && !oneOf(at(tokens, index - 1), {".", "->", "::"})) {
   while (oneOf(at(tokens, end + 1), {".", "->"}) && identifier(at(tokens, end + 2)) && at(tokens, end + 3) != "(") end += 2;
   if (oneOf(at(tokens, end + 1), {".", "->"}) && at(tokens, end + 3) == "(") {
    std::string key;
    for (auto part = index; part <= end; ++part) {
     if (part != index) key += ' ';
     key += tokens[part];
    }
    result.push_back(identity(key));
    index = end;
    continue;
   }
  }
  const bool local = text.size() > 1 && text[0] == '$' && std::all_of(text.begin() + 1, text.end(), [](unsigned char ch) { return digit(ch); });
  result.push_back(local ? identity(text) : text);
 }
 return result;
}
template <class Scope>
std::vector<std::ptrdiff_t> scopeOwners(const std::vector<Scope>& scopes, std::size_t size) {
 std::vector<std::ptrdiff_t> owners(size, -1);
 std::vector<std::size_t> active;
 std::size_t next = 0;
 for (std::size_t index = 0; index < size; ++index) {
  while (!active.empty() && scopes[active.back()].end <= index) active.pop_back();
  while (next < scopes.size() && scopes[next].open == index) active.push_back(next++);
  if (!active.empty()) owners[index] = static_cast<std::ptrdiff_t>(active.back());
 }
 return owners;
}
}  // namespace
std::u16string toUtf16(std::string_view source) {
 std::u16string result;
 result.reserve(source.size());
 for (std::size_t index = 0; index < source.size();) {
  const auto lead = static_cast<unsigned char>(source[index++]);
  std::uint32_t point = lead;
  unsigned count = 0;
  if (lead < 0x80) {
  } else if (lead >= 0xc2 && lead <= 0xdf) {
   point &= 0x1f;
   count = 1;
  } else if (lead >= 0xe0 && lead <= 0xef) {
   point &= 0x0f;
   count = 2;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
   point &= 0x07;
   count = 3;
  } else {
   result += u'\ufffd';
   continue;
  }
  unsigned consumed = 0;
  while (consumed < count && index < source.size()) {
   const auto byte = static_cast<unsigned char>(source[index]);
   if ((byte & 0xc0) != 0x80 || (consumed == 0 && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte >= 0x90)))) break;
   point = (point << 6) | (byte & 0x3f);
   ++index;
   ++consumed;
  }
  if (consumed != count) {
   result += u'\ufffd';
   continue;
  }
  if (point <= 0xffff)
   result += static_cast<char16_t>(point);
  else {
   point -= 0x10000;
   result += static_cast<char16_t>(0xd800 | (point >> 10));
   result += static_cast<char16_t>(0xdc00 | (point & 0x3ff));
  }
 }
 return result;
}
std::string fromUtf16(std::u16string_view source) {
 std::string result;
 result.reserve(source.size());
 for (std::size_t index = 0; index < source.size(); ++index) {
  std::uint32_t point = source[index];
  if (highSurrogate(source[index]) && index + 1 < source.size() && lowSurrogate(source[index + 1])) point = 0x10000 + ((point - 0xd800) << 10) + (source[++index] - 0xdc00);
  appendUtf8(result, point);
 }
 return result;
}
std::string jsTrim(std::string_view source) {
 const auto units = toUtf16(source);
 return fromUtf16(text::trimUnits(units));
}
std::vector<Token> tokenizeCpp(std::string_view source, bool trivia) {
 const auto units = toUtf16(source);
 return tokenizeCpp(std::u16string_view(units), trivia);
}
std::vector<Token> tokenizeCpp(std::u16string_view source, bool trivia) {
 std::vector<Token> result;
 for (std::size_t offset = 0; offset < source.size();) {
  auto end = offset;
  std::string kind = "code";
  const auto ch = source[offset];
  if (whitespace(ch)) {
   while (end < source.size() && whitespace(source[end])) ++end;
   kind = "whitespace";
  } else if (source.substr(offset, 2) == u"//") {
   end = lineEnd(source, offset + 2);
   kind = "comment";
  } else if (source.substr(offset, 2) == u"/*") {
   end = source.find(u"*/", offset + 2);
   end = end == std::u16string_view::npos ? source.size() : end + 2;
   kind = "comment";
  } else {
   end = rawStringEnd(source, offset);
   if (end != offset)
    kind = "literal";
   else {
    const auto quote = stringQuote(source, offset);
    if (quote < source.size() && source[quote] == u'"') {
     const auto close = quotedEnd(source, quote);
     if (close != quote) {
      end = close;
      kind = "literal";
     }
    }
    if (end == offset && ch == u'\'') {
     end = quotedEnd(source, offset);
     if (end != offset) kind = "literal";
    }
   }
   if (end == offset) {
    end = offset + 1;
    if (digit(ch)) {
     // Preserve the original regex's greedy first alternative: e/p are
     // word characters, so a following sign starts a separate token.
     while (end < source.size() && (letter(source[end]) || digit(source[end]) || source[end] == u'_' || source[end] == u'.' || source[end] == u'\'')) ++end;
    } else if (letter(ch) || ch == u'_' || ch == u'$') {
     while (end < source.size() && word(source[end])) ++end;
    } else {
     const auto pair = source.substr(offset, 2);
     if (pair == u"::" || pair == u"->" || pair == u"==" || pair == u"!=" || pair == u"<=" || pair == u">=" || pair == u"&&" || pair == u"||" || pair == u"++" || pair == u"--" || pair == u"<<" ||
         pair == u">>")
      ++end;
     if (ch == u'"' || ch == u'\'') kind = "literal";
    }
   }
  }
  if (trivia || (kind != "whitespace" && kind != "comment")) result.push_back({fromUtf16(source.substr(offset, end - offset)), offset, end, std::move(kind)});
  offset = end;
 }
 return result;
}
std::vector<Token> sourceComments(std::string_view path, std::string_view source) {
 if (!path.ends_with(".rs")) {
  auto tokens = tokenizeCpp(source, true);
  std::erase_if(tokens, [](const Token& token) { return token.kind != "comment"; });
  return tokens;
 }
 const auto storage = toUtf16(source);
 const std::u16string_view units = storage;
 std::vector<Token> comments;
 const auto hex = [](char16_t ch) { return digit(ch) || (ch >= u'a' && ch <= u'f') || (ch >= u'A' && ch <= u'F'); };
 for (std::size_t offset = 0; offset < units.size();) {
  auto end = offset + 1;
  bool comment = false;
  if (units.substr(offset, 2) == u"//") {
   end = lineEnd(units, offset + 2);
   comment = true;
  } else if (units.substr(offset, 2) == u"/*") {
   std::size_t depth = 1;
   end = offset + 2;
   while (end < units.size() && depth != 0) {
    if (units.substr(end, 2) == u"/*") {
     ++depth;
     end += 2;
    } else if (units.substr(end, 2) == u"*/") {
     --depth;
     end += 2;
    } else
     ++end;
   }
   comment = true;
  } else {
   auto prefix = offset;
   if (units.substr(offset, 2) == u"br" || units.substr(offset, 2) == u"cr")
    prefix += 2;
   else if (units[offset] == u'r')
    ++prefix;
   bool raw = false;
   if (prefix != offset) {
    auto quote = prefix;
    while (quote < units.size() && units[quote] == u'#') ++quote;
    if (quote < units.size() && units[quote] == u'"') {
     std::u16string terminator = u"\"";
     terminator.append(quote - prefix, u'#');
     const auto close = units.find(terminator, quote + 1);
     end = close == std::u16string_view::npos ? units.size() : close + terminator.size();
     raw = true;
    }
   }
   if (!raw) {
    auto quote = offset;
    if (units[offset] == u'b' || units[offset] == u'c') ++quote;
    bool literal = false;
    if (quote < units.size() && units[quote] == u'"') {
     const auto close = quotedEnd(units, quote);
     if (close != quote) {
      end = close;
      literal = true;
     }
    }
    if (!literal) {
     quote = offset + (units[offset] == u'b' ? 1 : 0);
     if (quote < units.size() && units[quote] == u'\'' && quote + 1 < units.size()) {
      auto close = quote + 1;
      if (units[close] == u'\\') {
       ++close;
       if (close < units.size() && units[close] != u'\r' && units[close] != u'\n') {
        auto escapedEnd = close + 1;
        if (units.substr(close, 2) == u"u{") {
         auto digitsEnd = close + 2;
         while (digitsEnd < units.size() && hex(units[digitsEnd])) ++digitsEnd;
         if (digitsEnd > close + 2 && digitsEnd < units.size() && units[digitsEnd] == u'}') escapedEnd = digitsEnd + 1;
        } else if (units[close] == u'x' && close + 2 < units.size() && hex(units[close + 1]) && hex(units[close + 2]))
         escapedEnd = close + 3;
        // Alternatives can backtrack to the generic one-character escape.
        if (escapedEnd < units.size() && units[escapedEnd] == u'\'')
         close = escapedEnd;
        else
         close += 1;
       }
      } else if (units[close] != u'\'' && units[close] != u'\r' && units[close] != u'\n')
       ++close;
      else
       close = units.size();
      if (close < units.size() && units[close] == u'\'') end = close + 1;
     }
    }
   }
  }
  if (comment) comments.push_back({fromUtf16(units.substr(offset, end - offset)), offset, end, "comment"});
  offset = end;
 }
 return comments;
}
std::pair<std::size_t, std::size_t> sourceRange(std::string_view source, const Json& occurrence, const std::vector<std::size_t>& lines) {
 const auto units = toUtf16(source);
 return sourceRange(std::u16string_view(units), occurrence, lines);
}
std::pair<std::size_t, std::size_t> sourceRange(std::u16string_view source, const Json& occurrence, const std::vector<std::size_t>& lines) {
 const auto field = [&](const char* key, Json fallback = nullptr) {
  const auto found = occurrence.find(key);
  return found == occurrence.end() ? fallback : *found;
 };
 const auto startValue = field("start"), endValue = field("end");
 const auto columnValue = field("column", 1);
 const bool hasEndColumn = occurrence.contains("endColumn");
 const auto endColumnValue = field("endColumn");
 const auto display = [](const Json& value, std::string_view missing) {
  if (value.is_discarded()) return std::string(missing);
  return value.is_string() ? value.get<std::string>() : value.dump();
 };
 const auto fail = [&]() -> void {
  const auto path = field("path");
  throw std::runtime_error("invalid source span: " + (path.is_null() ? std::string("source") : display(path, "source")) + ":" +
                           (occurrence.contains("start") ? display(startValue, "undefined") : "undefined") + ":" + display(columnValue, "1") + "-" +
                           (occurrence.contains("end") ? display(endValue, "undefined") : "undefined") + ":" + (endColumnValue.is_null() ? std::string("EOL") : display(endColumnValue, "EOL")));
 };
 const auto safeInteger = [](const Json& value) {
  if (!value.is_number()) return false;
  const double number = value.get<double>();
  return std::isfinite(number) && std::trunc(number) == number && std::abs(number) <= 9007199254740991.0;
 };
 if (!safeInteger(startValue) || !safeInteger(endValue) || !safeInteger(columnValue) || startValue.get<double>() < 1 || endValue.get<double>() < startValue.get<double>() ||
     columnValue.get<double>() < 1 || lines.empty())
  fail();
 const auto start = startValue.get<std::size_t>();
 const auto end = endValue.get<std::size_t>();
 const auto column = columnValue.get<std::size_t>();
 const auto physicalLines = lines.size() - (source.ends_with(u'\n') ? 1 : 0);
 if (end > physicalLines) fail();
 const auto contentEnd = [&](std::size_t line) {
  auto offset = line < lines.size() ? lines[line] - 1 : source.size();
  if (offset > 0 && source[offset - 1] == u'\r') --offset;
  return offset;
 };
 const auto startOffset = lines[start - 1] + column - 1;
 std::size_t endOffset;
 if (hasEndColumn) {
  if (!safeInteger(endColumnValue) || endColumnValue.get<double>() < 1) fail();
  endOffset = lines[end - 1] + endColumnValue.get<std::size_t>() - 1;
 } else
  endOffset = end < lines.size() ? lines[end] : source.size();
 if (startOffset >= contentEnd(start) || (hasEndColumn && endOffset > contentEnd(end)) || endOffset <= startOffset) fail();
 for (const auto offset : {startOffset, endOffset}) {
  if (offset > 0 && offset < source.size() && highSurrogate(source[offset - 1]) && lowSurrogate(source[offset])) fail();
 }
 return {startOffset, endOffset};
}
SourceIndex::SourceIndex(std::string_view text) : source(text), sourceUnits_(toUtf16(text)) {
 const std::u16string_view units = sourceUnits_;
 lines = text::lineStarts(units);
 tokens = tokenizeCpp(std::u16string_view(units));
 std::vector<std::size_t> stack;
 for (std::size_t index = 0; index < tokens.size(); ++index) {
  const auto& token = tokens[index].text;
  if (oneOf(token, {"(", "[", "{"}))
   stack.push_back(index);
  else if (oneOf(token, {")", "]", "}"})) {
   const auto opening = token == ")" ? "(" : token == "]" ? "[" : "{";
   if (stack.empty()) {
    balanced = false;
    continue;
   }
   const auto open = stack.back();
   stack.pop_back();
   if (tokens[open].text != opening) {
    balanced = false;
    continue;
   }
   pairs.emplace(open, index);
   pairs.emplace(index, open);
  }
 }
 balanced = balanced && stack.empty();
 indexFunctions();
 owners = scopeOwners(functions, tokens.size());
 recordOwners = scopeOwners(records, tokens.size());
 indexStatements();
 indexLocals();
}
void SourceIndex::indexStatements() {
 const auto size = tokens.size();
 statementStarts.assign(size, 0);
 statementEnds.assign(size, 0);
 std::unordered_set<std::size_t> scopes;
 for (const auto& fn : functions) scopes.insert(fn.open);
 for (const auto& record : records) scopes.insert(record.open);
 std::size_t start = 0;
 const auto finish = [&](std::size_t end) {
  std::fill(statementStarts.begin() + static_cast<std::ptrdiff_t>(start), statementStarts.begin() + static_cast<std::ptrdiff_t>(end), start);
  std::fill(statementEnds.begin() + static_cast<std::ptrdiff_t>(start), statementEnds.begin() + static_cast<std::ptrdiff_t>(end), end);
  start = end;
 };
 for (std::size_t index = 0; index < size; ++index) {
  const auto& text = tokens[index].text;
  const auto prior = at(tokens, index - 1);
  const bool block = text == "{" && (scopes.contains(index) || owners[index] < 0 || oneOf(prior, {")", "{", "}", ";", "else", "try", "do", ":"}));
  // Parenthesized expressions, for headers and aggregate initializers belong
  // to their complete statement, including the callee and every argument.
  if (text == "(" || text == "[" || (text == "{" && !block)) {
   const auto close = pairs.find(index);
   if (close != pairs.end() && close->second > index) {
    index = close->second;
    continue;
   }
  }
  if (text == ";" || text == "}" || block) finish(index + 1);
 }
 finish(size);
}
void SourceIndex::indexFunctions() {
 std::size_t boundary = 0;
 for (std::size_t index = 0; index < tokens.size(); ++index) {
  const auto& text = tokens[index].text;
  if (text == "(" || text == "[") {
   const auto close = pairs.find(index);
   if (close != pairs.end() && close->second > index) {
    index = close->second;
    continue;
   }
  }
  if (text != "{") {
   if (text == ";" || text == "}") boundary = index + 1;
   continue;
  }
  const auto closing = pairs.find(index);
  if (closing == pairs.end()) continue;
  const auto end = closing->second;
  for (auto header = boundary; header + 1 < index; ++header) {
   if (!oneOf(tokens[header].text, {"class", "struct", "union"})) continue;
   const auto& name = tokens[header + 1].text;
   if (identifier(name) && oneOf(at(tokens, header + 2), {"{", "final", ":"})) {
    records.push_back({name, boundary, index, end, std::nullopt});
    break;
   }
  }
  const auto parameter = findParameters(boundary, index);
  if (parameter) {
   auto parameters = parameterDeclarations(tokens, pairs, parameter->open + 1, parameter->close);
   const auto firstParameter = tokenText(parameters.front(), 0, parameters.front().size());
   auto firstStatementEnd = index + 1;
   while (firstStatementEnd < end && firstStatementEnd - index <= 128) {
    const auto& current = tokens[firstStatementEnd].text;
    if (current == ";") break;
    if (current == "{") {
     if (oneOf(at(tokens, index + 1), {"if", "for", "while", "switch", "try", "{"})) break;
     const auto close = pairs.find(firstStatementEnd);
     if (close == pairs.end() || close->second - index > 128) break;
     firstStatementEnd = close->second + 1;
     continue;
    }
    ++firstStatementEnd;
   }
   std::optional<std::string> firstStatement;
   if (at(tokens, firstStatementEnd) == ";") firstStatement = tokenText(tokens, index + 1, firstStatementEnd + 1);
   bool guard = false;
   if (firstStatement) {
    std::string_view setupText = *firstStatement;
    if (setupText.starts_with("const ")) setupText.remove_prefix(6);
    if (setupText.starts_with("std :: ")) {
     setupText.remove_prefix(7);
     for (const std::string_view name : {"scoped_lock", "lock_guard", "unique_lock", "shared_lock"}) {
      if (!setupText.starts_with(name)) continue;
      // JS \b uses ASCII word characters, excluding '$'.
      guard = setupText.size() == name.size() || !word(static_cast<unsigned char>(setupText[name.size()])) || setupText[name.size()] == '$';
      if (guard) break;
     }
    }
   }
   const bool setup = firstStatement && firstStatement->find('(') != std::string::npos && !nonType(at(tokens, index + 1)) && !guard;
   Json parameterTypes = Json::array();
   for (const auto& declaration : parameters) {
    Json type = Json::array();
    const auto size = declaration.size() - (parameterName(declaration).has_value() ? 1 : 0);
    for (std::size_t n = 0; n < size; ++n) type.push_back(declaration[n].text);
    parameterTypes.push_back(std::move(type));
   }
   Function fn;
   fn.name = parameter->name;
   fn.start = boundary;
   fn.open = index;
   fn.end = end;
   fn.firstStatementEnd = firstStatementEnd;
   fn.parameters = std::move(parameters);
   fn.parameterTypes = json_dump(parameterTypes);
   if (guard) fn.guardEnd = firstStatementEnd;
   if (setup && !firstParameter.empty() && firstParameter != "void") fn.prefix = json_dump(Json::array({fn.name, firstParameter, *firstStatement}));
   functions.push_back(std::move(fn));
  }
  boundary = index + 1;
 }
}
void SourceIndex::indexLocals() {
 for (auto& fn : functions) {
  for (const auto& declaration : fn.parameters) {
   const auto name = parameterName(declaration);
   if (name) fn.locals.insert(*name);
  }
 }
 for (std::size_t index = 1; index + 1 < tokens.size(); ++index) {
  const auto owner = owners[index];
  if (owner < 0) continue;
  const auto& name = tokens[index].text;
  if (!identifier(name) || !oneOf(tokens[index + 1].text, {"=", "{", ";", ":"})) continue;
  auto prior = index - 1;
  while (prior < tokens.size() && oneOf(at(tokens, prior), {"*", "&", "&&", "const"})) --prior;
  const auto type = at(tokens, prior);
  if ((identifier(type) && !nonType(type)) || type == ">" || type == ">>") functions[static_cast<std::size_t>(owner)].locals.insert(name);
 }
}
std::optional<Parameters> SourceIndex::findParameters(std::size_t start, std::size_t end) const {
 for (auto index = start; index < end; ++index) {
  if (tokens[index].text != "(") continue;
  const auto closing = pairs.find(index);
  if (closing == pairs.end() || closing->second >= end) continue;
  const auto close = closing->second;
  std::string name(at(tokens, index - 1));
  if (name == ")" && at(tokens, index - 3) == "operator") name = "operator()";
  if (name == "]" && at(tokens, index - 3) == "operator") name = "operator[]";
  if (name == "operator" && at(tokens, close + 1) == "(") {
   index = close;
   continue;
  }
  if (at(tokens, index - 2) == "operator") name = "operator" + name;
  const bool suffix = close + 1 == end || oneOf(at(tokens, close + 1), {"const", "volatile", "&", "&&", "noexcept", "override", "final", "->", "requires", ":", "["});
  if (suffix && ((identifier(name) && !control(name) && !oneOf(name, {"static_cast", "dynamic_cast", "reinterpret_cast", "const_cast"})) || name.starts_with("operator")))
   return Parameters{name, index, close};
  index = close;
 }
 return std::nullopt;
}
std::pair<std::size_t, std::size_t> SourceIndex::range(const Json& occurrence) const {
 const auto [startOffset, endOffset] = sourceRange(std::u16string_view(sourceUnits_), occurrence, lines);
 const auto lower = [&](std::size_t offset) {
  return static_cast<std::size_t>(std::lower_bound(tokens.begin(), tokens.end(), offset, [](const Token& token, std::size_t value) { return token.offset < value; }) - tokens.begin());
 };
 return {lower(startOffset), lower(endOffset)};
}
Json SourceIndex::occurrence(std::string_view path, std::size_t start, std::size_t end) const {
 if (start >= tokens.size() || end <= start || end > tokens.size()) throw std::out_of_range("invalid token occurrence range");
 const auto& first = tokens[start];
 const auto& last = tokens[end - 1];
 const auto startLine = static_cast<std::size_t>(std::lower_bound(lines.begin(), lines.end(), first.offset + 1) - lines.begin());
 const auto endLine = static_cast<std::size_t>(std::lower_bound(lines.begin(), lines.end(), last.offset + 1) - lines.begin());
 return {{"path", path}, {"start", startLine}, {"end", endLine}, {"column", first.offset - lines[startLine - 1] + 1}, {"endColumn", last.end - lines[endLine - 1] + 1}};
}
bool SourceIndex::isBinding(std::size_t start, std::size_t end, const Function& fn) const {
 if (end <= start || end > tokens.size() || tokens[end - 1].text != ";") return false;
 auto equal = start;
 while (equal < end && tokens[equal].text != "=") ++equal;
 const auto nameIndex = equal == end ? end - 2 : equal - 1;
 if (nameIndex <= start || nameIndex >= end || !fn.locals.contains(tokens[nameIndex].text) || nonType(tokens[start].text)) return false;
 for (auto index = start; index < nameIndex; ++index)
  if (oneOf(tokens[index].text, {".", "->", "[", "{", "}"})) return false;
 if (equal == end) return true;
 if (end - equal == 3) return true;
 std::string rhs;
 for (auto index = equal + 1; index + 1 < end; ++index) rhs += tokens[index].text;
 std::size_t offset = 0, components = 0;
 while (offset < rhs.size()) {
  const auto first = offset;
  while (offset < rhs.size() && word(static_cast<unsigned char>(rhs[offset]))) ++offset;
  if (offset == first) return false;
  ++components;
  if (offset == rhs.size()) break;
  if (rhs[offset] == '.')
   ++offset;
  else if (std::string_view(rhs).substr(offset, 2) == "->" || std::string_view(rhs).substr(offset, 2) == "::")
   offset += 2;
  else
   return false;
  if (offset == rhs.size()) return false;
 }
 return components >= 2;
}
Json SourceIndex::describe(const Json& occurrenceValue) {
 const auto [rawStart, rawEnd] = range(occurrenceValue);
 const auto start = rawStart < statementStarts.size() ? statementStarts[rawStart] : rawStart;
 const auto end = rawEnd > 0 && rawEnd - 1 < statementEnds.size() ? statementEnds[rawEnd - 1] : rawEnd;
 auto description = describeTokens(occurrenceValue, rawStart, rawEnd, start, end);
 description["parts"] = Json::array();
 std::set<std::ptrdiff_t> intersected;
 for (auto index = start; index < end; ++index)
  if (owners[index] >= 0) intersected.insert(owners[index]);
 for (const auto owner : intersected) {
  const auto& fn = functions[static_cast<std::size_t>(owner)];
  const auto first = std::max(start, fn.open + 1);
  const auto last = std::min(end, fn.end);
  if (last <= first) continue;
  const std::set<std::size_t> starts{first, statementEnds[first]};
  const std::set<std::size_t> ends{last, statementStarts[last - 1]};
  for (const auto from : starts)
   for (const auto to : ends) {
    if (to <= from || from < first || to > last) continue;
    bool complete = true;
    for (auto at = from; at < to; ++at) {
     const auto pair = pairs.find(at);
     if (pair != pairs.end() && (pair->second < from || pair->second >= to)) {
      complete = false;
      break;
     }
    }
    if (!complete) continue;
    auto part = describeTokens(occurrenceValue, rawStart, rawEnd, from, to);
    if (part.at("key").is_null()) continue;
    part["occurrence"] = occurrence(occurrenceValue.at("path").get<std::string>(), from, to);
    description["parts"].push_back(std::move(part));
   }
 }
 return description;
}
Json SourceIndex::describeTokens(const Json& occurrenceValue, std::size_t rawStart, std::size_t rawEnd, std::size_t start, std::size_t end) {
 Json prefixes = Json::array();
 std::vector<std::string> calls, assertions, declarations, executable, namedExecutable;
 std::set<std::size_t> declarationRecords;
 std::unordered_map<std::string, std::string> localNames;
 std::optional<std::size_t> declarationStart;
 if (start == 0 || oneOf(tokens[start - 1].text, {";", "{", "}"})) declarationStart = start;
 std::size_t executableTokens = 0, maxStatements = 0;
 std::unordered_set<std::size_t> seenFunctions;
 std::unordered_map<std::size_t, std::size_t> statementsByFunction;
 for (auto index = start; index < end; ++index) {
  const auto owner = owners[index];
  const auto& text = tokens[index].text;
  if (owner < 0) {
   if (text == ";" && declarationStart) {
    std::size_t identifiers = 0;
    bool excluded = false;
    for (auto fact = *declarationStart; fact < index; ++fact) {
     identifiers += identifier(tokens[fact].text) ? 1 : 0;
     excluded = excluded || oneOf(tokens[fact].text, {"(", ")", "public", "private", "protected", "using", "namespace", "import", "export", "typedef"});
    }
    if (identifiers >= 2 && !excluded) {
     declarations.push_back(tokenText(tokens, *declarationStart, index + 1));
     if (recordOwners[index] >= 0) declarationRecords.insert(static_cast<std::size_t>(recordOwners[index]));
    }
   }
   if (oneOf(text, {";", "{", "}"})) declarationStart = index + 1;
   continue;
  }
  declarationStart.reset();
  const auto functionIndex = static_cast<std::size_t>(owner);
  const auto& fn = functions[functionIndex];
  if (index == fn.open) continue;
  if (seenFunctions.insert(functionIndex).second && fn.prefix && fn.firstStatementEnd >= rawStart && fn.firstStatementEnd < rawEnd && fn.firstStatementEnd + 1 < fn.end) {
   prefixes.push_back(
    {{"key", *fn.prefix}, {"start", fn.start}, {"end", fn.end}, {"parameterTypes", fn.parameterTypes},
     {"occurrence", occurrence(occurrenceValue.at("path").get<std::string>(), fn.start, fn.firstStatementEnd + 1)}});
  }
  ++executableTokens;
  namedExecutable.push_back(text);
  const bool qualified = oneOf(at(tokens, index - 1), {".", "->", "::"}) || at(tokens, index + 1) == "::";
  if (fn.locals.contains(text) && !qualified) {
   const auto identity = std::to_string(owner) + ":" + text;
   auto found = localNames.find(identity);
   if (found == localNames.end()) {
    const auto name = "$" + std::to_string(localNames.size());
    found = localNames.emplace(identity, name).first;
   }
   executable.push_back(found->second);
  } else
   executable.push_back(text);
  if (text == ";" && statementEnds[index] == index + 1 && (!fn.guardEnd || index > *fn.guardEnd)) {
   const auto first = statementStarts[index];
   const bool transfer = index == first + 1 && oneOf(tokens[first].text, {"return", "break", "continue"});
   auto& count = statementsByFunction[functionIndex];
   if (!transfer && !isBinding(first, index + 1, fn)) ++count;
   maxStatements = std::max(maxStatements, count);
  }
  if (!identifier(text) || at(tokens, index + 1) != "(" || control(text)) continue;
  const auto close = pairs.find(index + 1);
  if (assertion(text)) {
   if (close != pairs.end() && close->second < end) assertions.push_back(tokenText(tokens, index, close->second + 1));
  } else {
   auto name = text;
   if (at(tokens, index - 1) == "::") {
    auto prior = index - 2;
    while (prior >= start && prior < tokens.size() && identifier(tokens[prior].text)) {
     name = tokens[prior].text + "::" + name;
     if (at(tokens, prior - 1) != "::") break;
     prior -= 2;
    }
   }
   if (name != "std::move" && name != "std::forward") calls.push_back(std::move(name));
  }
 }
 const auto operation = [](const std::string& text) { return oneOf(text, {"if", "for", "while", "switch", "return", "throw", "+", "-", "/", "%", "*"}); };
 const bool independentReads = !calls.empty() && std::all_of(calls.begin(), calls.end(), [](const std::string& name) { return oneOf(name, {"size", "empty", "data"}); }) &&
                               !std::any_of(executable.begin(), executable.end(), operation);
 const bool arithmetic = assertions.empty() && std::any_of(executable.begin(), executable.end(), [](const std::string& text) { return oneOf(text, {"+", "-", "*", "/", "%"}); });
 // Ordinary member facts and alias relations survive receiver normalization;
 // independent storage/dimension reads also retain their original local roles.
 const auto operations = independentReads ? namedExecutable : arithmetic ? algorithmReceivers(executable) : executable;
 Record* record = declarations.size() >= 2 && declarationRecords.size() == 1 ? &records[*declarationRecords.begin()] : nullptr;
 Json key = nullptr;
 const bool expression = maxStatements == 1 && assertions.empty() && !executable.empty() && executable.front() == "return" &&
                         std::count_if(executable.begin(), executable.end(), [](const std::string& text) { return oneOf(text, {"&&", "||", "+", "-", "*", "/", "%", "<<", ">>", "&", "|"}); }) >= 2;
 if (maxStatements >= 2 || expression) {
  if (!calls.empty())
   key = json_dump(Json::array({"operations", operations}));
  else if (assertions.size() >= 2)
   key = json_dump(Json::array({"assertions", executable}));
  else if (std::any_of(executable.begin(), executable.end(), operation))
   key = json_dump(Json::array({"statements", executable}));
 } else if (record != nullptr)
  key = recordKey(*record);
 const bool useRecord = maxStatements < 2 && record != nullptr;
 const auto path = occurrenceValue.contains("path") ? occurrenceValue.at("path") : Json(nullptr);
 const auto identity = json_dump(Json::array({path, useRecord ? record->start : start, useRecord ? record->end : end}));
 return {
  {"prefixes", std::move(prefixes)}, {"key", std::move(key)}, {"identity", identity}, {"statements", maxStatements}, {"expression", expression}, {"executableTokens", executableTokens},
  {"balanced", balanced}
 };
}
std::string SourceIndex::recordKey(Record& record) {
 if (!record.key) {
  Json values = Json::array();
  for (auto index = record.start; index < record.end; ++index) values.push_back(tokens[index].text == record.name ? "$self" : tokens[index].text);
  record.key = json_dump(Json::array({"record", values}));
 }
 return *record.key;
}
Json filterCpdCandidates(const Json& candidates, const FilterOptions& options) {
 struct Entry {
  Json occurrence;
  Json description;
  std::size_t original = 0;
  std::string parameterTypes;
 };
 struct Group {
  std::string key;
  std::vector<const Entry*> entries;
  Json targets = Json::array();
 };
 const auto materialize = [](const std::string& key, const auto& identities) {
  Group group{key, {}};
  for (const auto& [identity, entry] : identities.entries) {
   if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(entry)>>)
    group.entries.push_back(entry);
   else
    group.entries.push_back(&entry);
   group.targets.push_back(identity);
  }
  return group;
 };
 const auto matchingGroups = [&](const auto& groups) {
  std::vector<Group> result;
  for (const auto& [key, identities] : groups.entries)
   if (identities.size() >= 2) result.push_back(materialize(key, identities));
  return result;
 };
 std::vector<std::vector<Entry>> described(candidates.size());
 OrderedMap<std::vector<std::pair<std::size_t, std::size_t>>> byPath;
 std::size_t nextIdentity = 0;
 for (std::size_t index = 0; index < candidates.size(); ++index) {
  for (const auto& occurrenceValue : candidates[index].at("occurrences")) {
   const auto entryIndex = described[index].size();
   described[index].push_back({occurrenceValue, nullptr, nextIdentity++, {}});
   byPath.get(occurrenceValue.at("path").get<std::string>()).emplace_back(index, entryIndex);
  }
 }
 // A single file's token index is live at once. File reads and source indexing
 // are shared by every occurrence, regardless of the candidate count.
 for (const auto& [path, entries] : byPath.entries) {
  const auto source = options.sourceReader ? options.sourceReader(path) : read_text(path);
  SourceIndex index(source);
  for (const auto& [candidate, entryIndex] : entries) {
   auto& entry = described[candidate][entryIndex];
   entry.description = index.describe(entry.occurrence);
  }
 }
 Json retained = Json::array(), filtered = Json::array(), reasons = Json::object();
 for (std::size_t index = 0; index < candidates.size(); ++index) {
  const auto& duplication = candidates[index];
  const auto& entries = described[index];
  std::unordered_set<std::size_t> accepted;
  const auto keep = [&](const std::vector<const Entry*>& group, std::string_view pattern, Json identities, const std::string& patternKey) {
   auto hit = duplication;
   hit["occurrences"] = Json::array();
   for (const auto* entry : group) {
    accepted.insert(entry->original);
    hit["occurrences"].push_back(entry->occurrence);
   }
   hit["patterns"] = Json::array({pattern});
   hit["targetIdentities"] = std::move(identities);
   hit["patternKey"] = patternKey;
   retained.push_back(std::move(hit));
  };
  OrderedMap<OrderedMap<const Entry*>> groups;
  for (const auto& entry : entries) {
   if (entry.description.at("key").is_null()) continue;
   groups.get(entry.description.at("key").get<std::string>()).set(entry.description.at("identity").get<std::string>(), &entry);
  }
  for (auto& group : matchingGroups(groups)) {
   const auto& description = group.entries.front()->description;
   keep(group.entries, (description.at("statements").get<std::size_t>() >= 2 || description.at("expression").get<bool>()) ? "shared executable operations" : "same complete record declaration",
    std::move(group.targets), group.key);
  }
  OrderedMap<OrderedMap<Entry>> prefixes;
  for (const auto& entry : entries) {
   for (const auto& prefix : entry.description.at("prefixes")) {
    Entry prefixed{prefix.at("occurrence"), nullptr, entry.original, prefix.at("parameterTypes").get<std::string>()};
    const auto identity = json_dump(Json::array({entry.occurrence.at("path"), prefix.at("start"), prefix.at("end")}));
    prefixes.get(prefix.at("key").get<std::string>()).set(identity, std::move(prefixed));
   }
  }
  for (auto& group : matchingGroups(prefixes)) {
   std::unordered_set<std::string> parameterTypes;
   for (const auto* entry : group.entries) parameterTypes.insert(entry->parameterTypes);
   if (parameterTypes.size() < 2) continue;
   keep(group.entries, "same function, first parameter and opening statement", std::move(group.targets), "prefix:" + group.key);
  }
  if (accepted.size() < 2) {
   OrderedMap<OrderedMap<Entry>> parts;
   for (const auto& entry : entries) {
    for (const auto& part : entry.description.at("parts")) {
     parts.get(part.at("key").get<std::string>()).set(part.at("identity").get<std::string>(), Entry{part.at("occurrence"), part, entry.original, {}});
    }
   }
   auto partGroups = matchingGroups(parts);
   std::stable_sort(partGroups.begin(), partGroups.end(), [](const Group& left, const Group& right) {
    return left.entries.front()->description.at("executableTokens").get<std::size_t>() > right.entries.front()->description.at("executableTokens").get<std::size_t>();
   });
   std::vector<Json> covered;
   for (auto& group : partGroups) {
    std::vector<Json> spans;
    for (const auto& identity : group.targets) spans.push_back(Json::parse(identity.get<std::string>()));
    const bool contained = std::all_of(spans.begin(), spans.end(),
     [&](const Json& span) { return std::any_of(covered.begin(), covered.end(), [&](const Json& prior) { return prior[0] == span[0] && prior[1] <= span[1] && prior[2] >= span[2]; }); });
    if (contained) continue;
    keep(group.entries, "shared complete statements within a larger match", std::move(group.targets), group.key);
    covered.insert(covered.end(), spans.begin(), spans.end());
   }
  }
  if (std::any_of(entries.begin(), entries.end(), [](const Entry& entry) { return !entry.description.at("balanced").get<bool>(); })) {
   Json identities = Json::array();
   std::vector<const Entry*> group;
   for (const auto& entry : entries) {
    identities.push_back(json_dump(entry.occurrence));
    group.push_back(&entry);
   }
   keep(group, "unclassified syntax requires manual review", std::move(identities), "unclassified:" + std::to_string(index));
  }
  if (accepted.size() >= 2) {
   Json unmatched = Json::array();
   for (const auto& entry : entries)
    if (!accepted.contains(entry.original)) unmatched.push_back(entry.occurrence);
   if (!unmatched.empty()) {
    auto rejected = duplication;
    rejected["occurrences"] = std::move(unmatched);
    rejected["reason"] = "occurrences without the retained group's operation or declaration identity";
    filtered.push_back(std::move(rejected));
   }
  } else {
   std::unordered_set<std::string> identities;
   bool declarationsOnly = true, singleOperation = true;
   for (const auto& entry : entries) {
    identities.insert(entry.description.at("identity").get<std::string>());
    declarationsOnly = declarationsOnly && entry.description.at("executableTokens").get<std::size_t>() == 0;
    singleOperation = singleOperation && entry.description.at("statements").get<std::size_t>() < 2;
   }
   const std::string reason = identities.size() < 2 ? "one containing statement or declaration reported more than once"
                              : declarationsOnly    ? "declaration or signature surface"
                               : singleOperation    ? "single operation or partial statement"
                                                    : "different complete operations, types, constants or assertion facts";
   reasons[reason] = reasons.value(reason, std::size_t{0}) + 1;
   auto rejected = duplication;
   rejected["reason"] = reason;
   filtered.push_back(std::move(rejected));
  }
 }
 if (!options.consolidate) return {{"duplications", std::move(retained)}, {"filtered", std::move(filtered)}, {"reasons", std::move(reasons)}, {"indexedFiles", byPath.size()}};
 struct Target {
  Json duplication;
  OrderedMap<Json> occurrences;
 };
 OrderedMap<Target> targets;
 for (const auto& retainedHit : retained) {
  auto duplication = retainedHit;
  const auto identities = duplication.at("targetIdentities");
  const auto key = duplication.at("patternKey").get<std::string>();
  duplication.erase("targetIdentities");
  duplication.erase("patternKey");
  OrderedMap<Json> occurrences;
  for (std::size_t index = 0; index < identities.size(); ++index) occurrences.set(identities[index].get<std::string>(), duplication.at("occurrences")[index]);
  auto* prior = targets.find(key);
  if (prior == nullptr)
   targets.set(key, Target{std::move(duplication), std::move(occurrences)});
  else {
   // Missing numeric metadata behaves like Math.max(undefined, ...): NaN
   // becomes null on JSON serialization. Normal detector candidates supply
   // both counts; retaining the behavior also supports direct API callers.
   const auto numeric = [](const Json& object, const char* name) {
    if (!object.contains(name)) return std::numeric_limits<double>::quiet_NaN();
    const auto& value = object.at(name);
    if (value.is_number()) return value.get<double>();
    if (value.is_null()) return 0.0;
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    return std::numeric_limits<double>::quiet_NaN();
   };
   const auto count = numeric(duplication, "tokenCount");
   const auto priorCount = numeric(prior->duplication, "tokenCount");
   for (auto& [identity, occurrenceValue] : occurrences.entries)
    if (prior->occurrences.find(identity) == nullptr || count > priorCount) prior->occurrences.set(identity, std::move(occurrenceValue));
   for (const auto* name : {"tokenCount", "lineCount"}) {
    const auto previous = numeric(prior->duplication, name), current = numeric(duplication, name);
    if (std::isnan(previous) || std::isnan(current))
     prior->duplication[name] = std::numeric_limits<double>::quiet_NaN();
    else if (current > previous)
     prior->duplication[name] = duplication.at(name);
    else if (!prior->duplication.contains(name) || !prior->duplication[name].is_number())
     prior->duplication[name] = previous;
   }
   duplication["reason"] = "same operation pattern already represented by a consolidated match";
   filtered.push_back(std::move(duplication));
  }
 }
 Json duplications = Json::array();
 for (auto& [key, target] : targets.entries) {
  target.duplication["occurrences"] = Json::array();
  for (auto& [identity, occurrenceValue] : target.occurrences.entries) target.duplication["occurrences"].push_back(std::move(occurrenceValue));
  duplications.push_back(std::move(target.duplication));
 }
 return {{"duplications", std::move(duplications)}, {"filtered", std::move(filtered)}, {"reasons", std::move(reasons)}, {"indexedFiles", byPath.size()}};
}
}  // namespace mmltk::tools::cpd
