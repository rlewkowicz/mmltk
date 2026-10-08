module;
#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <compare>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <set>
#include <tuple>

export module mmltk.tools.log_query;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

export {
namespace mmltk::log_query {
using Json = mmltk::tools::json;
using Big = boost::multiprecision::cpp_int;
namespace fs = std::filesystem;
inline const Json MISSING = Json(Json::value_t::discarded);
inline constexpr std::size_t MAX_QUERY_LENGTH = 8192, MAX_QUERY_TOKENS = 512,
    MAX_QUERY_DEPTH = 32, MAX_LINE_BYTES = 1024 * 1024, MAX_MESSAGE_LINES = 128,
    MAX_VULKAN_OBJECTS = 64, MAX_DISTINCT_KEYS = 10000, MAX_TRIAGE_TEXT = 2048,
    MAX_TRIAGE_STATES = 256, MAX_TRIAGE_EVENTS = 64, MAX_TRIAGE_PIXEL_SAMPLES = 16384;

struct QueryError : std::runtime_error { using runtime_error::runtime_error; };
bool missing(const Json& value);
bool exact(const Json& left, const Json& right);
Json scalar(std::string_view text, bool quoted = false);
Json parse_json(std::string_view text);
std::string encoded(const Json& value);
std::string compact(const Json& value, std::optional<std::size_t> width = 180);
std::string normalized_text(std::string_view value);
Json lookup(const Json& data, std::string_view name);
Json value_from(const Json& data, std::initializer_list<std::string_view> names);

struct Record {
  std::string source;
  std::size_t line = 1;
  std::string raw;
  Json data = Json::object();
  std::string format = "text", parse_error;
  Json metadata = Json::object();
  std::string clock = "none";
  std::optional<Big> time_ns;
  Record() = default;
  Record(std::string source, std::size_t line, std::string raw, Json data,
         std::string format, std::string parse_error = {}, Json metadata = Json::object());
  bool training_record() const;
  Json get(std::string_view name) const;
};
std::string record_text(const Record& record);
Record parse_record(std::string source, std::size_t line, std::string raw, bool truncated = false);
Record triage_snapshot(const Record& record);
std::pair<std::string, std::string> event_stage(const Record& record);
bool triage_terminal(const Record& record);

struct Expression {
  std::string operation = "all";
  Json arguments = Json::array();
  std::vector<Expression> children;
  bool matches(const Record& record) const;
};
class QueryParser {
public:
  explicit QueryParser(std::string source);
  Expression parse();
private:
  struct Token { std::string text; bool quoted; };
  std::vector<Token> tokens_;
  std::size_t index_ = 0;
  bool peek(std::string_view value) const;
  Token take();
  void require(std::string_view value);
  Expression parse_or(std::size_t depth);
  Expression parse_and(std::size_t depth);
  Expression primary(std::size_t depth);
};

struct Options {
  std::vector<std::string> paths, correlate, family, fields, stats;
  std::vector<std::string> group_by{"@owner", "@event"};
  std::string query, where, format = "summary", order, run;
  std::optional<std::string> error;
  std::optional<bool> auto_correlate;
  bool errors = false, triage = false, descriptor_lineage = false,
       representatives = false, tail = false, recursive = false, history = false,
       list_runs = false, related_run = false, strict = false, help = false;
  int triage_anchors = 4, triage_identities = 12, triage_hops = 2,
      triage_gap_ms = 1000, near_ms = 1000, context = 0, top = 0, limit = 20,
      last_lines = 0, bucket_ms = 0;
};
Options parse_options(const std::vector<std::string>& arguments);
std::vector<std::string> field_names(std::string_view text, char separator = ',');
std::optional<std::vector<std::string>> correlation_key(const Record&, const std::vector<std::string>&);
Json order_key(const Record&, std::string_view order);
Json physical_position(const Record&);

using Anchors = std::map<std::string, Json>;
struct LogFile {
  fs::path path;
  std::string source;
  std::uint64_t device = 0, inode = 0, size = 0;
  Big modified_ns = 0;
  Json metadata = Json::object();
  std::vector<std::string> linked_runs;
  std::uint64_t start_offset = 0;
  std::size_t first_line = 1;
  static LogFile capture(const fs::path&, const fs::path& root);
  LogFile recent(std::size_t count) const;
  void records(const std::function<void(Record)>& visitor, bool unchanged = false,
               const Anchors& anchors = {},
               const std::function<bool(std::string_view)>& line_hint = {}) const;
};

struct Identity {
  std::string run;
  std::vector<std::string> names, values;
  auto operator<=>(const Identity&) const = default;
  std::string label() const;
};
std::vector<Identity> strong_identities(const Record&, const std::vector<std::vector<std::string>>& explicit_fields = {});
using AutoFacts = std::pair<std::map<std::string, std::string>, std::vector<std::pair<int, Identity>>>;
AutoFacts auto_facts(const Record&);
bool auto_correlate_enabled(const Options&);

struct Retained {
  Json key = Json::array();
  Record record;
  std::string reason;
  bool tail = false;
  int priority = 0;
  Big distance = 0;
};
Json projected(const Record&, const std::vector<std::string>& names);
std::string render_record(const Retained&, const Options&);

// Arguments exclude argv[0]. An empty root resolves the native source's repository.
int main(const std::vector<std::string>& arguments, const fs::path& root,
         std::ostream& output, std::ostream& diagnostics);
} // namespace mmltk::log_query

}

export namespace mmltk::log_query {
int entry(int argc, char** argv);
int test_entry(int argc, char** argv);
}
