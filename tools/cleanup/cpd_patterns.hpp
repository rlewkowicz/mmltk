#pragma once

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
std::pair<std::size_t, std::size_t> sourceRange(
    std::string_view source, const Json& occurrence, const std::vector<std::size_t>& lines);
std::pair<std::size_t, std::size_t> sourceRange(
    std::u16string_view source, const Json& occurrence, const std::vector<std::size_t>& lines);

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
  Json describeTokens(const Json& occurrence, std::size_t rawStart,
                      std::size_t rawEnd, std::size_t start, std::size_t end);
  std::u16string sourceUnits_;
};

struct FilterOptions {
  // An empty callback selects the native file reader.
  std::function<std::string(const std::string&)> sourceReader;
  bool consolidate = true;
};

Json filterCpdCandidates(const Json& candidates, const FilterOptions& options = {});

}  // namespace mmltk::tools::cpd
