module;
// The same-module fixture partition observes the one production implementation.
// Limits and observation sinks are scoped to the selected fixture thread.
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
module mmltk.tools.log_query:tests;
import std;
import mmltk.tools.log_query;
import :internal;
import mmltk.tools.tool_command;
import mmltk.tools.diagnose_native_process;
import mmltk.tools.cpp_linux;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.runtime_package;
import mmltk.tools.tests.cpp_test_support;
namespace log_query_test {
using Json = mmltk::log_query::Json;
using mmltk::log_query::detail::Observation;
using mmltk::log_query::detail::observations;
using mmltk::log_query::detail::observed_events;
struct Observe {
 std::vector<Observation> events;
 std::set<std::string_view> allowed;
 std::vector<Observation>* previous = observations;
 const std::set<std::string_view>* previous_events = observed_events;
 explicit Observe(std::initializer_list<std::string_view> names = {}) : allowed(names) {
  observations = &events;
  observed_events = allowed.empty() ? nullptr : &allowed;
 }
 Observe(const Observe&) = delete;
 Observe& operator=(const Observe&) = delete;
 ~Observe() {
  observations = previous;
  observed_events = previous_events;
 }
 std::size_t count(std::string_view event) const {
  return std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.event == event; });
 }
 std::size_t maximum(std::string_view event) const {
  std::size_t maximum = 0;
  for (const auto& e : events)
   if (e.event == event) maximum = std::max(maximum, e.value.get<std::size_t>());
  return maximum;
 }
};
struct Limit {
 std::size_t& target;
 std::size_t old;
 Limit(std::size_t& target, std::size_t value) : target(target), old(target) { target = value; }
 Limit(const Limit&) = delete;
 Limit& operator=(const Limit&) = delete;
 ~Limit() { target = old; }
};
}  // namespace log_query_test
namespace log_query_test {
namespace logs = mmltk::log_query;
namespace support = mmltk::tools;
namespace fs = std::filesystem;
using J = Json;
using Strings = std::vector<std::string>;
using Rows = std::vector<logs::Record>;
struct Failure : std::runtime_error {
 using runtime_error::runtime_error;
};
inline void require(bool condition, std::string_view expression, int line) {
 if (!condition) throw Failure("line " + std::to_string(line) + ": " + std::string(expression));
}
inline bool equal(const J& a, const J& b) { return (logs::missing(a) && logs::missing(b)) || logs::exact(a, b); }
template <class A, class B>
bool equal(const A& a, const B& b) {
 if constexpr (std::integral<A> && std::integral<B> && !std::same_as<A, bool> && !std::same_as<B, bool>)
  return std::cmp_equal(a, b);
 else
  return a == b;
}
inline bool truth(const J& value) { return logs::truth(value); }
template <class T>
bool truth(const T& value) {
 if constexpr (requires { value.empty(); })
  return !value.empty();
 else
  return static_cast<bool>(value);
}
#define CHECK(...) require(truth((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
#define FALSE(...) require(!truth((__VA_ARGS__)), "not " #__VA_ARGS__, __LINE__)
#define EQ(a, ...) require(equal((a), (__VA_ARGS__)), #a " == " #__VA_ARGS__, __LINE__)
#define NE(a, ...) require(!equal((a), (__VA_ARGS__)), #a " != " #__VA_ARGS__, __LINE__)
inline bool contains(std::string_view text, std::string_view needle) { return text.find(needle) != std::string_view::npos; }
template <class T>
 requires std::convertible_to<const T&, std::string_view>
inline bool contains(const T& text, std::string_view needle) {
 return contains(std::string_view(text), needle);
}
inline bool contains(const J& text, std::string_view needle) { return text.is_string() && contains(text.get_ref<const std::string&>(), needle); }
#define HAS(text, needle) CHECK(contains((text), (needle)))
#define LACKS(text, needle) FALSE(contains((text), (needle)))
template <class E = logs::QueryError, class F>
void raises(F&& fn, std::string_view message = {}) {
 try {
  fn();
 } catch (const E& error) {
  require(contains(error.what(), message), "exception message", __LINE__);
  return;
 }
 throw Failure("expected exception: " + std::string(message));
}
struct Case {
 std::string_view name;
 void (*run)();
 bool tools_ownership = false;
};
J j(std::string_view text) { return J::parse(text); }
std::string fixture_json(const J& value) { return value.dump(-1, ' ', true); }
J array(std::size_t size) {
 J result = J::array();
 for (std::size_t i = 0; i < size; ++i) result.push_back(i);
 return result;
}
J updated(J value, const J& changes) {
 value.update(changes);
 return value;
}
std::string repeat(std::string_view text, std::size_t count) {
 std::string result;
 for (std::size_t i = 0; i < count; ++i) result += text;
 return result;
}
std::string padded(std::string text, std::size_t count, char ch) {
 if (text.size() < count) text.resize(count, ch);
 return text;
}
std::size_t occurrences(std::string_view text, std::string_view needle) {
 std::size_t count = 0, offset = 0;
 while ((offset = text.find(needle, offset)) != std::string_view::npos) {
  ++count;
  offset += needle.size();
 }
 return count;
}
logs::Record record(const J& data, std::string source = "runtime.jsonl", std::size_t line = 1) { return logs::parse_record(std::move(source), line, fixture_json(data)); }
std::string firefox_fatal(std::string component = "firefox.webgpu", std::string operation = "signal_retire", int pid = 202, int ppid = 101, int pgid = 202,
 std::string context = "actor=7 device=9 submission=11 ", std::string status = "ERROR_DEVICE_LOST") {
 return "fatal: " + component + ": operation=" + operation + " pid=" + std::to_string(pid) + " ppid=" + std::to_string(ppid) + " pgid=" + std::to_string(pgid) + " " + context + "status=" + status;
}
std::string vulkan_message(std::string identifier = "VUID-vkCmdCopyImageToBuffer-srcImage-00186", std::string handle = "0x001AbC", std::string object_type = "IMAGE", std::optional<int> pid = {},
 std::string level = "ERROR", std::string detail = "The Vulkan spec states: srcImage must have transfer source usage.") {
 const std::string module = "wgpu_hal::vulkan::instance";
 const std::string prefix = pid ? "[Child " + std::to_string(*pid) + ": Renderer]: E/" + module : "[" + level + " " + module + "]";
 return prefix + " VALIDATION [" + identifier + " (0x30009145)]\n    \tValidation " + (level == "WARN" ? "Warning" : "Error") + ": [ " + identifier + " ] Object 0: handle = " + handle +
        ", type = VK_OBJECT_TYPE_" + object_type + "; | MessageID = 0x30009145 | vkCmdCopyImageToBuffer(): " + detail + "\n" + prefix + " \tobjects: (type: " + object_type + ", hndl: " + handle +
        ", name: ?)\n";
}
bool matches(std::string expression, const J& data) { return logs::QueryParser(std::move(expression)).parse().matches(record(data)); }
Rows parsed(logs::TranscriptContext& context, std::string source, const Strings& lines, std::size_t start = 1) {
 Rows result;
 for (const auto& line : lines) {
  auto rows = context.parse_line(source, start++, line, J::object(), {});
  result.insert(result.end(), rows.begin(), rows.end());
 }
 return result;
}
template <class Range, class F>
bool all(const Range& range, F fn) {
 return std::all_of(range.begin(), range.end(), fn);
}
template <class Range, class F>
bool any(const Range& range, F fn) {
 return std::any_of(range.begin(), range.end(), fn);
}
template <class Range, class F>
auto select(const Range& range, F fn) {
 using T = typename Range::value_type;
 std::vector<T> result;
 for (const auto& item : range)
  if (fn(item)) result.push_back(item);
 return result;
}
template <class Range, class F>
auto project(const Range& range, F fn) {
 using T = std::decay_t<decltype(fn(*range.begin()))>;
 std::vector<T> result;
 for (const auto& item : range) result.push_back(fn(item));
 return result;
}
template <class Range, class F>
auto projected_set(const Range& range, F fn) {
 using T = std::decay_t<decltype(fn(*range.begin()))>;
 std::set<T> result;
 for (const auto& item : range) result.insert(fn(item));
 return result;
}
template <class Range>
const auto& only(const Range& range) {
 EQ(range.size(), 1U);
 return *range.begin();
}
template <class F>
void permutations(Strings values, F fn) {
 std::sort(values.begin(), values.end());
 do { fn(values); } while (std::next_permutation(values.begin(), values.end()));
}
void test_boolean_precedence_implicit_and_and_quoted_terms() {
 const auto data = j(R"json({"event":"worker.failure","owner":"explore","message":"CUDA error: device lost"})json");
 CHECK(matches(R"json(missing OR owner=explore "cuda ERROR" NOT retry)json", data));
 FALSE(matches(R"json((missing OR owner=other) AND "cuda error")json", data));
 CHECK(matches("NOT (owner=other OR event:ready)", data));
 CHECK(matches("'error: device' AND *", data));
 FALSE(matches(R"json("*")json", data));
}
void test_exact_numeric_values_preserve_uint64_and_boolean_type() {
 auto data = j(R"json({"trace_id":18446744073709551615,"value":1,"flag":true,"ratio":1.25})json");
 CHECK(matches("trace_id=18446744073709551615 ratio>=1.2", data));
 FALSE(matches("trace_id=18446744073709551614", data));
 FALSE(matches(R"json(trace_id="18446744073709551615")json", data));
 FALSE(matches("value=true OR flag=1 OR flag>=0", data));
 CHECK(matches("value!=true AND flag=true", data));
}
void test_presence_null_and_missing_comparisons() {
 auto data = j(R"json({"nil":null,"zero":0,"empty":""})json");
 CHECK(matches(R"json(has(nil) nil=null has(zero) empty="")json", data));
 FALSE(matches("absent!=0 OR absent!~anything", data));
 CHECK(matches("NOT absent=0 AND NOT has(absent)", data));
}
void test_substrings_regex_and_nested_fields() {
 auto data = j(R"json({"fields":{"name":"iced.draw.accepted","trace_id":88,"nested":{"value":17}}})json");
 CHECK(matches("fields.nested.value>16 trace_id=88 @event:ICED", data));
 CHECK(matches(R"json(@event~"^iced[.]draw" AND @event!~"failure$")json", data));
 CHECK(matches(R"json(@event~"(?i)ACCEPTED")json", data));
 FALSE(matches(R"json(@event~"ACCEPTED")json", data));
}
void test_invalid_queries_report_errors() {
 Strings invalid = {
  "(", ")", "()", "NOT", "AND error", "error OR", "error AND", "event=", "event==value", "duration_ns>slow", "duration_ns>true", "event~'[invalid'", "\"unclosed", "field='unclosed", "has()",
  "has(\"event\")", "\"event\"=start", "duration_ns>1e9999", "error ! failure", std::string(34, '(') + "error" + std::string(34, ')'), repeat("a ", 513), std::string(8193, 'a')
 };
 for (const auto& expression : invalid) raises([&] { logs::QueryParser(expression).parse(); });
}
void test_intent_request_projection_keeps_exact_uint64_and_rejects_other_counters() {
 auto maximum = j("18446744073709551615");
 for (auto event : {"received", "accepted", "rejected", "reply_rejected"})
  EQ(record({{"fields", {{"event", std::string("browser.intent.") + event}, {"value", maximum}}}}).get("@intent_request"), maximum);
 EQ(record({{"event", "integration.intent"}, {"control", fixture_json(maximum)}}).get("@intent_request"), maximum);
 auto invalid = j(R"json([0,-1,true,1.0,"","-1","0","18446744073709551616"])json");
 invalid.push_back(std::string(100, '1'));
 for (const auto& value : invalid) CHECK(logs::missing(record({{"event", "integration.intent"}, {"control", value}}).get("@intent_request")));
 CHECK(logs::missing(record(j(R"json({"event":"browser.server.binary_received","value":7})json")).get("@intent_request")));
}
void test_training_terminal_errors_keep_run_and_attempt() {
 for (const auto& item : j(R"json([["Terminal","Error",true],["Terminal","Completed",false],["Live","Train",false],["Live","Validate",false]])json")) {
  auto row = record({{"format_version", 3}, {"role", item[0]}, {"run_id", "run-a"}, {"attempt_id", "attempt-b"}, {"progress", {{"phase", item[1]}, {"class_error", 42.0}}}}, "metrics.jsonl", 3);
  EQ(row.get("@error"), item[2]);
  EQ(row.get("@terminal"), J(item[0] == "Terminal"));
  EQ(row.get("@owner"), J("training"));
  EQ(row.get("@event"), J("training." + logs::lower(item[0].get<std::string>()) + "." + logs::lower(item[1].get<std::string>())));
  EQ(row.get("run_id"), J("run-a"));
  EQ(row.get("attempt_id"), J("attempt-b"));
  EQ(row.get("@line"), J(3));
 }
 FALSE(record(j(R"json({"role":"Terminal","progress":{"phase":"Error"}})json")).get("@error"));
}
void test_training_session_model_and_first_cause_survive_bounded_triage() {
 auto row = record(j(
  R"json({"format_version":3,"role":"Terminal","run_id":"run-a","attempt_id":"attempt-b","progress":{"phase":"Error","session_id":"session-a","model_id":7,"failure":{"session_id":"session-a","model_id":7,"first_cause":19,"detail":"CUDA allocation failed"}}})json"));
 for (const auto& candidate : {row, logs::triage_snapshot(row)}) {
  EQ(candidate.get("@session"), J("session-a"));
  EQ(candidate.get("@model"), J(7));
  EQ(candidate.get("@first_cause"), J(19));
  CHECK(logs::QueryParser("@session=session-a @model=7 @first_cause=19 @error=true").parse().matches(candidate));
  auto names = projected_set(logs::strong_identities(candidate), [](auto& id) { return id.names; });
  CHECK(names.contains(Strings{"progress.session_id", "progress.model_id"}));
  CHECK(names.contains(Strings{"progress.session_id", "progress.failure.first_cause"}));
 }
}
void test_training_record_near_misses_do_not_project_lifecycle() {
 auto base = j(R"json({"format_version":3,"role":"Terminal","run_id":"run-a","attempt_id":"attempt-b","progress":{"phase":"Error"}})json");
 for (const auto& mutation :
  j(
   R"json([{"format_version":1},{"format_version":2},{"format_version":true},{"role":"terminal"},{"run_id":""},{"attempt_id":" "},{"attempt_id":1},{"progress":{"phase":"Unknown"}},{"progress":"Error"}])json")) {
  auto row = record(updated(base, mutation));
  FALSE(row.training_record());
  CHECK(logs::missing(row.get("@event")));
  FALSE(row.get("@error"));
  FALSE(row.get("@terminal"));
 }
 EQ(logs::compact(logs::MISSING), "(not recorded)");
}
void test_training_snapshot_reserves_canonical_facts_before_optional_payloads() {
 for (const auto& pair : j(R"json([["Boundary","Starting"],["Terminal","Completed"],["Live","Validate"]])json")) {
  J configuration = J::object();
  for (int i = 0; i < 200; ++i) configuration[std::to_string(i)] = std::string(4096, 'x');
  auto row = record({{"attempt_configuration", configuration}, {"progress", {{"metrics", array(200)}, {"phase", pair[1]}}}, {"format_version", 3}, {"role", pair[0]}, {"sequence", 7},
                     {"run_id", "run-a"}, {"attempt_id", "attempt-b"}},
   "metrics.jsonl");
  auto retained = logs::triage_snapshot(row);
  EQ(retained.get("@event"), J("training." + logs::lower(pair[0].get<std::string>()) + "." + logs::lower(pair[1].get<std::string>())));
  for (auto name : {"format_version", "role", "sequence", "run_id", "attempt_id", "progress.phase"}) EQ(retained.get(name), row.get(name));
  EQ(logs::event_stage(retained), (std::pair<std::string, std::string>{"", ""}));
  FALSE(retained.get("@error"));
  EQ(retained.source, "metrics.jsonl");
  EQ(retained.line, 1U);
  CHECK(retained.metadata["triage_payload_truncated"]);
  CHECK(fixture_json(retained.data).size() < 96 * (2 * logs::MAX_TRIAGE_TEXT + 16));
  EQ(row.data["attempt_configuration"].size(), 200U);
  EQ(row.data["progress"]["metrics"].size(), 200U);
 }
}
void test_complete_jsonl_decodes_each_payload_once() {
 J data = {{"event", "sample"}, {"fields", {{"nested", array(100)}}}};
 Observe decoded({"json.decode"});
 auto row = record(data);
 EQ(row.data, data);
 FALSE(row.parse_error);
 EQ(decoded.count("json.decode"), 1U);
}
void test_runtime_and_wrapped_json_keep_original_vocabulary() {
 auto row = record(j(R"json({"event":"browser.telemetry","fields":{"name":"iced.draw.accepted","trace_id":52,"steady_ns":123}})json"));
 EQ(row.get("event"), J("browser.telemetry"));
 EQ(row.get("@event"), J("iced.draw.accepted"));
 EQ(row.get("trace_id"), J(52));
 EQ(row.get("@owner"), J("iced"));
 EQ(row.clock, "steady");
 EQ(row.time_ns, std::optional<logs::Big>(123));
}
void test_surface_identity_has_one_exact_projection() {
 auto native = record({{"surface_high", UINT64_C(0xFEDCBA9876543210)}, {"surface_low", UINT64_C(0x1234567890ABCDEF)}});
 auto firefox = record({{"surface", "FEDCBA98765432101234567890ABCDEF"}});
 EQ(native.get("@surface"), firefox.get("@surface"));
 EQ(native.get("@surface"), J("fedcba98765432101234567890abcdef"));
 CHECK(logs::missing(record({{"surface_high", -1}, {"surface_low", 5}}).get("@surface")));
 CHECK(logs::missing(record({{"surface", "12"}}).get("@surface")));
}
void test_workspace_source_is_separate_from_artifact_and_arena() {
 auto native = record(j(R"json({"workspace_source_high":11,"workspace_source_low":12,"surface_high":21,"surface_low":22})json"));
 auto browser = record(j(R"json({"source":"000000000000000b000000000000000c","surface":"00000000000000150000000000000016"})json"), "firefox.log");
 EQ(native.get("@workspace_source"), browser.get("@workspace_source"));
 EQ(native.get("@surface"), browser.get("@surface"));
 NE(native.get("@surface"), native.get("@workspace_source"));
 EQ(browser.source, "firefox.log");
 CHECK(logs::missing(record({{"source", "wrong"}}).get("@workspace_source")));
 CHECK(logs::missing(record(j(R"json({"source":"000000000000000b000000000000000c","workspace_source_high":11,"workspace_source_low":13})json")).get("@workspace_source")));
}
void test_transfer_correlation_uses_source_and_rejects_arena_counter_aliases() {
 auto identities = [](int source, int arena = 20, int transfer = 3) {
  std::set<logs::Identity> result;
  auto row = record(
   {{"event", "presentation.release_wait.completed"}, {"workspace_source_high", 1}, {"workspace_source_low", source}, {"surface_high", 1}, {"surface_low", arena}, {"transfer_sequence", transfer},
    {"presentation_revision", 7}});
  for (const auto& [priority, id] : logs::auto_facts(row).second)
   if (std::find(id.names.begin(), id.names.end(), "transfer_sequence") != id.names.end()) result.insert(id);
  return result;
 };
 auto first = identities(10);
 CHECK(first);
 CHECK(all(first, [](auto& id) { return std::find(id.names.begin(), id.names.end(), "@workspace_source") != id.names.end(); }));
 for (const auto& other : {identities(11), identities(10, 20, 4)}) FALSE(any(other, [&](auto& id) { return first.contains(id); }));
 EQ(first, identities(10, 21));
}
void test_memory_provenance_process_ids_never_expand_as_resource_identities() {
 auto row = record(j(R"json({"event":"gpu.workspace.memory_export","native_process_id":101,"browser_process_id":202,"workspace_descriptor":9,"workspace_plane":4096})json"));
 FALSE(logs::strong_identities(row));
 Strings fields = {"native_process_id", "workspace_descriptor", "workspace_plane"};
 EQ(project(logs::strong_identities(row, {fields}), [](auto& id) { return id.names; }), std::vector<Strings>{fields});
}
void test_native_header_and_key_value_messages() {
 auto row = logs::parse_record(
  "native.log", 4, "1970-01-01 00:00:01.123456789 [694:836] [mmltk-browser-host] [debug] event=execution.placement owner=compiled-read device=0 cpu=2 detail='two words' failed=false");
 EQ(row.format, "native");
 EQ(row.get("pid"), J(694));
 EQ(row.get("tid"), J(836));
 EQ(row.get("@event"), J("execution.placement"));
 EQ(row.get("@owner"), J("compiled-read"));
 EQ(row.get("detail"), J("two words"));
 EQ(row.get("device"), J(0));
 EQ(row.get("failed"), J(false));
 EQ(row.clock, "wall-local");
 EQ(row.time_ns, std::optional<logs::Big>(1123456789));
}
void check_fatal_envelope(const logs::Record& row) {
 EQ(row.format, "fatal");
 EQ(row.get("@event"), J("fatal.report"));
 EQ(row.get("@level"), J("fatal"));
 CHECK(row.get("@error"));
}
void test_native_fatal_components_and_detail_keep_the_reporter_bounds() {
 auto detail = padded(R"json({"pid":999,"event":"shadow","trace_id":42} pid=888 pgid=777)json", 800, 'x') + "...";
 for (const auto& component : Strings{"cli", std::string(128, 'x'), std::string(129, 'x'), std::string(160, 'x'), std::string(160, 'x') + "..."}) {
  for (const auto& status : {J(nullptr), J("-2147483648")}) {
   const auto suffix = status.is_null() ? "" : " (status=" + status.get<std::string>() + ")";
   const auto message = "fatal: " + component + ": " + detail + suffix;
   CHECK(message.size() + 1 < 1024);
   auto row = logs::parse_record("native.log", 4, message);
   check_fatal_envelope(row);
   EQ(row.get("component"), J(component));
   EQ(row.get("detail"), J(detail + suffix));
   EQ(row.get("message"), J(message));
   EQ(row.raw, message);
   EQ(row.get("status"), status.is_null() ? logs::MISSING : status);
   FALSE(row.parse_error);
   for (auto field : {"pid", "pgid", "trace_id", "message_fields", "@process", "@process_group"}) CHECK(logs::missing(row.get(field)));
  }
 }
}
void test_firefox_fatal_envelopes_preserve_context_and_opaque_status() {
 const auto fixtures = j(
  R"json([["firefox.webgpu","signal_retire","actor=7 device=9 submission=11 ",{"actor":7,"device":9,"submission":11}],["firefox.ipc","channel_error","actor=0x123 channel=PWebGPU side=child ",{"actor":"0x123","channel":"PWebGPU","side":"child"}]])json");
 for (const auto& fixture : fixtures)
  for (const auto& status : Strings{"ERROR_DEVICE_LOST", R"json({"pid":999,"pgid":888,"event":"shadow"})json", "pid=999 ppid=888 pgid=777 actor=99 trace_id=42 status=original (detail)"})
   for (const auto& prefix : Strings{"", "1970-01-01 00:00:01 [694:836] [mmltk] [critical] ", "[Child 202: Renderer]: E/wgpu "}) {
    auto message = firefox_fatal(fixture[0].get<std::string>(), fixture[1].get<std::string>(), 202, 101, 202, fixture[2].get<std::string>(), status);
    auto row = logs::parse_record("firefox.log", 5, prefix + message);
    check_fatal_envelope(row);
    EQ(row.raw, prefix + message);
    EQ(row.get("message"), J(message));
    EQ(row.get("status"), J(status));
    EQ(row.get("operation"), fixture[1]);
    FALSE(row.parse_error);
    auto expected = updated(fixture[3], {{"@process", 202}, {"@parent_process", 101}, {"@process_group", 202}});
    for (auto it = expected.begin(); it != expected.end(); ++it) EQ(row.get(it.key()), it.value());
    for (auto name : {"trace_id", "message_fields", "process_id", "process_group_id"}) CHECK(logs::missing(row.get(name)));
    if (contains(prefix, "[694:836]")) {
     EQ(row.get("logger"), J("mmltk"));
     EQ(row.get("tid"), J(836));
    }
   }
}
void test_process_projections_require_exact_consistent_observed_ids() {
 for (const auto& [projection, fields] :
  std::map<std::string, Strings>{{"@process", {"pid", "process_id", "browser_process_id"}}, {"@parent_process", {"ppid", "parent_process_id"}}, {"@process_group", {"pgid", "process_group_id"}}}) {
  for (const auto& value : j(R"json([1,202,2147483647,"202","2147483647"])json")) {
   J data = J::object();
   for (auto& field : fields) data[field] = value;
   EQ(record(data).get(projection), value.is_string() ? j(value.get<std::string>()) : value);
  }
  for (const auto& value : j(R"json([0,-1,true,202.0,"","0","0202","+202","202.0",2147483648,"2147483648"])json")) {
   J data = J::object();
   for (auto& field : fields) data[field] = value;
   CHECK(logs::missing(record(data).get(projection)));
  }
  for (int second : {303, 0}) CHECK(logs::missing(record({{fields[0], 202}, {fields[1], second}}).get(projection)));
 }
 for (auto event : {"child.spawned", "stop.term_selected", "stop.kill_selected", "child.exited", "child.signaled"})
  for (const auto& sequence : j(R"json([202,"202",0,"0"])json")) {
   auto row = record({{"owner", "firefox_process"}, {"event", event}, {"sequence", sequence}, {"pid", 999}, {"ppid", 888}, {"pgid", 777}});
   auto expected = sequence == 202 || sequence == "202" ? J(202) : logs::MISSING;
   EQ(row.get("@process"), expected);
   EQ(row.get("@process_group"), expected);
   CHECK(logs::missing(row.get("@parent_process")));
  }
 CHECK(logs::missing(record(j(R"json({"event":"child.exited","owner":"training","sequence":202})json"), "firefox-child-202.log").get("@process")));
}
void test_vulkan_report_conditions_do_not_become_allocation_or_fd_identity_facts() {
 const std::string message =
  "[ERROR wgpu_hal::vulkan::instance] VALIDATION [VUID-VkMemoryAllocateInfo-allocationSize-01742 (0xc093a791)]\n    \tValidation Error: [ VUID-VkMemoryAllocateInfo-allocationSize-01742 ] | MessageID "
  "= 0xc093a791 | vkAllocateMemory(): allocationSize (2097152) does not match fd (122) allocationSize (0). The Vulkan spec states: If the external handle specified was created by the Vulkan API, "
  "then the values of allocationSize and memoryTypeIndex must match.";
 auto row = logs::parse_record("firefox.log", 10, message);
 EQ(row.get("api"), J("vkAllocateMemory"));
 HAS(row.get("message"), "If the external handle specified was created by the Vulkan API");
 EQ(row.raw, message);
 EQ(row.get("objects"), J::array());
 CHECK(logs::missing(row.get("workspace_descriptor")));
 CHECK(logs::missing(row.get("memory_size")));
 FALSE(logs::vulkan_resources(row.data));
 FALSE(logs::strong_identities(row));
}
void test_vulkan_object_projection_keeps_types_and_multiple_images_separate() {
 const auto message =
  support::text::trim_newlines(vulkan_message()) +
  "\n    Object 1: handle = 0x20, type = VK_OBJECT_TYPE_IMAGE; Object 2: handle = 0x1abc, type = VK_OBJECT_TYPE_DEVICE_MEMORY; Object 3: handle = 0x0, type = VK_OBJECT_TYPE_IMAGE;";
 auto row = logs::parse_record("firefox.log", 1, message);
 CHECK(logs::missing(row.get("vk_image")));
 EQ(row.get("vk_memory"), J("0x1abc"));
 auto resources = logs::vulkan_resources(row.data);
 EQ((std::set<std::pair<std::string, std::string>>(resources.begin(), resources.end())),
  (std::set<std::pair<std::string, std::string>>{{"vk_image", "0x1abc"}, {"vk_image", "0x20"}, {"vk_memory", "0x1abc"}}));
 CHECK(any(row.get("objects"), [](const auto& value) { return value == j(R"json({"type":"IMAGE","handle":"0x0"})json"); }));
 {
  Limit limit(mmltk::log_query::detail::limits.vulkan_objects, 1);
  auto bounded = logs::parse_record("firefox.log", 1, message);
  EQ(bounded.get("objects").size(), 1U);
  HAS(bounded.parse_error, "bounded capacity");
  EQ(bounded.raw, message);
 }
 CHECK(logs::missing(record(j(R"json({"objects":[{"type":[],"handle":"0x20"}]})json")).get("vk_image")));
}
void test_firefox_moz_log_including_optional_timestamp() {
 const std::string message = "[Child 780: IPC I/O Child]: E/ipc OnChannelErrorFromLink";
 auto row = logs::parse_record("firefox.log", 31, message);
 EQ(row.format, "firefox");
 EQ(row.get("pid"), J(780));
 EQ(row.get("thread"), J("IPC I/O Child"));
 EQ(row.get("@owner"), J("ipc"));
 EQ(row.get("@level"), J("error"));
 CHECK(row.get("@error"));
 EQ(row.clock, "none");
 auto timed = logs::parse_record("firefox.log", 32, "1970-01-01 00:00:01.000001 UTC " + message);
 EQ(timed.clock, "wall");
 EQ(timed.time_ns, std::optional<logs::Big>(1000001000));
}
void test_embedded_json_and_unstructured_records_keep_provenance() {
 auto row = logs::parse_record("firefox.log", 5, R"json(console.error: {"event":"integration.annotation_pixel","error":0,"matched":true})json");
 EQ(row.format, "json");
 FALSE(row.get("@error"));
 EQ(row.source, "firefox.log");
 EQ(row.line, 5U);
 CHECK(row.raw.starts_with("console.error:"));
 auto plain = logs::parse_record("acceptance.out", 6, "\x1b[31mFAILED: require ready\x1b[0m");
 CHECK(plain.get("@error"));
 EQ(plain.raw, "FAILED: require ready");
}
void test_json_and_plain_failure_candidates() {
 FALSE(record(j(R"json({"event":"integration.pixel","error":0})json")).get("@error"));
 CHECK(record({{"event", "worker.failed"}}).get("@error"));
 CHECK(record(j(R"json({"level":"critical","message":"unavailable"})json")).get("@error"));
 CHECK(record({{"message", "operation timed out"}}).get("@error"));
 CHECK(logs::parse_record("test.log", 2, "Segmentation fault (core dumped)").get("@error"));
}
void test_malformed_json_remains_searchable() {
 for (const auto& [source, text] :
  std::vector<std::pair<std::string, std::string>>{{"trace.jsonl", "bad data"}, {"firefox.log", "{\"event\":\"bad\","}, {"trace.jsonl", "{\"value\":NaN}"}, {"trace.jsonl", "[]"}}) {
  auto row = logs::parse_record(source, 9, text);
  CHECK(row.parse_error);
  EQ(row.format, "text");
  EQ(row.get("@text"), J(text));
 }
}
void test_compiler_progress_and_source_keep_failure_vocabulary_as_context() {
 for (auto text : {"[3/594] Building CXX object image_failure.h.cpp.o", "[8/594] Building CXX object cuda_error.h.cpp.o", "   25 | host_ = {};", " 1120 | capture({}, {}); // error path"}) {
  auto row = logs::parse_record("tidy.log", 1, text);
  FALSE(row.get("@error"));
  FALSE(row.parse_error);
  EQ(row.get("message"), J(text));
 }
 for (auto text : {"FAILED: [code=1] image_failure.h.cpp.o", "predict.cpp:25:11: error: ambiguous overload for operator=", "ninja: build stopped: cannot make progress due to previous errors."})
  CHECK(logs::parse_record("tidy.log", 2, text).get("@error"));
}
void test_clock_domains_and_nanosecond_precision() {
 auto stamp = logs::timestamp_ns("1970-01-01T01:00:01.123456789+01:00");
 CHECK(stamp);
 EQ(stamp->first, "wall");
 EQ(stamp->second, logs::Big(1123456789));
 EQ(record(j(R"json({"timestamp_ns":9007199254740993})json")).time_ns, std::optional<logs::Big>(UINT64_C(9007199254740993)));
 auto elapsed = record({{"elapsed_ms", 1.234567}}, "firefox.log");
 EQ(elapsed.clock, "elapsed:firefox.log");
 EQ(elapsed.time_ns, std::optional<logs::Big>(1234567));
 EQ(record({{"timestamp", "not a timestamp"}}).clock, "none");
 EQ(record({{"time_ns", 55}}).clock, "none");
}
void test_non_scalar_event_and_overflowing_json_numbers_do_not_crash() {
 logs::TranscriptContext context;
 auto rows = parsed(context, "log.jsonl", {R"json({"event":{"name":"future"},"steady_ns":5})json"});
 EQ(rows[0].get("event.name"), J("future"));
 CHECK(logs::parse_record("log.jsonl", 2, R"json({"value":1e9999})json").parse_error);
 CHECK(logs::parse_record("log.jsonl", 3, R"json({"event":"one"} trailing)json").parse_error);
 auto row = logs::parse_record("log.log", 4, R"json({"event":"one"} {"event":"two"})json");
 EQ(row.get("@event"), J("one"));
 HAS(row.parse_error, "multiple JSON payloads");
}
void test_signal_semantics_preserve_encoded_child_exit_status() {
 for (const auto& [code, number, name] : std::vector<std::tuple<int, int, std::string>>{{139, 11, "SIGSEGV"}, {143, 15, "SIGTERM"}}) {
  auto row = record({{"event", "child.signaled"}, {"value", code}});
  EQ(row.get("@exit_code"), J(code));
  EQ(row.get("@signal_number"), J(number));
  EQ(row.get("@signal"), J(name));
  CHECK(row.get("@terminal"));
  CHECK(logs::QueryParser(name).parse().matches(row));
 }
 CHECK(logs::missing(record(j(R"json({"event":"child.exited","value":139})json")).get("@signal")));
 FALSE(record(j(R"json({"event":"child.exited","value":0})json")).get("@error"));
}
void test_transport_decode_and_allocator_failures_are_searchable_without_a_level() {
 for (auto message :
  {"invalid server CBOR record: UpscaleSnapshot.scene: invalid fixed text size: AnnotationText.size=0, expected 1..=96", "corrupted size vs. prev_size while consolidating",
   "free(): double free detected in tcache 2", "malloc(): corrupted top size"}) {
  auto row = logs::parse_record("stderr.log", 1, message);
  CHECK(row.get("@error"));
  CHECK(logs::anchor_rank(row).first >= 90);
 }
 FALSE(record(j(R"json({"event":"iced.transport.disconnected","message":"WebSocket closed: 1000"})json")).get("@error"));
}
void test_catch_failure_preserves_parent_and_extracts_quoted_json() {
 logs::TranscriptContext context;
 parsed(context, "test.log", {"[ RUN ] viewer_copy"});
 auto payload = j(R"json({"event":"firefox.webgpu.blocking_map","buffer":"BufferId(21,1)","detail":"arc_extract","strong_count":2,"settled":false})json");
 auto rows = parsed(context, "test.log", {"/workspace/test.cpp:17: failed: count == 1 with 1 message: " + fixture_json(J(fixture_json(payload)))}, 2);
 EQ(rows.size(), 2U);
 EQ(rows[0].get("@event"), J("catch.assertion_failed"));
 EQ(rows[0].get("source_line"), J(17));
 EQ(rows[1].get("buffer"), J("BufferId(21,1)"));
 EQ(rows[1].metadata["part"], J(1));
 EQ(rows[1].metadata["context_copy"], J("Catch INFO"));
 EQ(rows[1].line, 2U);
}
void test_wrapped_native_context_and_image_results() {
 auto row = logs::parse_record("test.log", 8, "' and 'native runtime log: 1970-01-01 00:00:01 [1:2] [host] [trace] event=onnx.completed");
 EQ(row.get("@event"), J("onnx.completed"));
 EQ(row.clock, "wall-local");
 auto image = logs::parse_record("build.log", 20, "#37 naming to docker.io/library/mmltk done");
 EQ(image.get("@event"), J("build.image"));
 CHECK(image.get("@terminal"));
 auto terminal = logs::parse_record("test.log", 21, "native host terminal status: exit 139");
 EQ(terminal.get("@exit_code"), J(139));
 CHECK(logs::missing(terminal.get("@signal")));
}
void test_catch_console_exception_keeps_location_and_placeholder_without_json_damage() {
 logs::TranscriptContext context;
 auto rows = parsed(context, "test.log",
  {"[ RUN ] copy_paste_cache", "/workspace/training.test.cpp:1009: FAILED:", "  {Unknown expression after the reported line}",
   "due to unexpected exception with message:", "  RF-DETR target staging capacity is exhausted", "[ FAILED ] copy_paste_cache"});
 CHECK(all(rows, [](auto& r) { return r.parse_error.empty(); }));
 CHECK(all(rows, [](auto& r) { return r.get("@test") == "copy_paste_cache"; }));
 EQ(rows[1].get("@event"), J("catch.assertion_failed"));
 EQ(rows[1].get("source_line"), J(1009));
 CHECK(rows[1].get("@error"));
 EQ(rows[2].get("@event"), J("catch.expression"));
 HAS(rows[4].get("message"), "staging capacity");
 CHECK(parsed(context, "test.log", {"  {\"event\":\"partial"}, 7)[0].parse_error);
}
void test_catch_report_title_recovers_delayed_stderr_test_identity() {
 logs::TranscriptContext context;
 auto rows = parsed(context, "test.log",
  {"[ RUN ] training failure before cleanup", "[ FAILED ] training failure before cleanup", "[ RUN ] unrelated preview", std::string(79, '-'), "training failure", "before cleanup",
   "  a selected section", std::string(79, '-'), "/workspace/training.test.cpp:1457", std::string(79, '.'), "/workspace/training.test.cpp:1479: FAILED:", "  trace file was not yet drained",
   "[ OK ] unrelated preview"});
 EQ(rows[10].get("@event"), J("catch.assertion_failed"));
 EQ(rows[10].get("@test"), J("training failure before cleanup"));
 EQ(rows[11].get("@test"), J("training failure before cleanup"));
 EQ(rows.back().get("@test"), J("unrelated preview"));
}
logs::Triage triage_fixture(bool capture = true) {
 auto options = logs::parse_options({"--triage", "--triage-identities", "64"});
 logs::Triage triage(options, logs::Expression{}, {});
 if (capture) triage.run = "capture";
 return triage;
}
struct PixelTriageFixture {
 Limit limit;
 logs::Triage triage;
 explicit PixelTriageFixture(std::size_t capacity, bool capture = true) : limit(mmltk::log_query::detail::limits.pixel_samples, capacity), triage(triage_fixture(capture)) {}
};
void expect_no_pending_pixels(const logs::Triage& triage) {
 FALSE(triage.pixel_pending);
 EQ(triage.pixel_pending_count, 0U);
}
logs::Record row(const J& data, std::size_t line = 1, std::string source = "input.jsonl") {
 auto result = record(data, std::move(source), line);
 result.metadata.update({{"run", "capture"}, {"mtime_ns", 1}});
 return result;
}
logs::Record pixel_row(std::string boundary, int transfer = 1, std::uint64_t rgba = 10, std::size_t line = 1, int index = 0) {
 std::string event = boundary == "forwarded"                  ? "firefox.workspace.frame_forwarded"
                     : boundary == "native"                   ? "presentation.pixel"
                      : boundary == "native_after_release"    ? "presentation.pixel_after_release"
                       : boundary.starts_with("source_copy_") ? "presentation.pixel_" + boundary
                        : boundary == "direct_image"          ? "firefox.workspace.direct_pixel"
                         : boundary == "sample"               ? "iced.surface.pixel"
                                                              : "firefox.workspace.pixel";
 J data = {
  {"surface", "00000000000000010000000000000002"}, {"presentation_revision", 1}, {"source", "00000000000000030000000000000004"}, {"sample_index", index}, {"sample_x", 4 + index % 5},
  {"sample_y", 5 + index / 5}, {"sample_rgba", rgba}, {"transfer_sequence", transfer}, {"boundary", boundary}, {"event", event}
 };
 if (boundary == "direct_image") data.update({{"completion", "Ok(true)"}, {"readback_marker", 0x4d4d4c54}, {"expected_marker", 0x4d4d4c54}});
 if (boundary == "import" || boundary == "mailbox") data.erase("source");
 return row(data, line);
}
std::vector<logs::TriageFinding> findings(const logs::Triage& triage, std::string_view kind = {}) {
 std::vector<logs::TriageFinding> result;
 for (const auto& [key, finding] : triage.findings)
  if (kind.empty() || finding.kind == kind) result.push_back(finding);
 return result;
}
std::vector<logs::TriageFinding> expect_native_pixel_comparison(const logs::Triage& triage, bool differs) {
 auto native = findings(triage, "pixel-chain-divergence");
 EQ(native.size(), static_cast<std::size_t>(differs));
 if (!native.empty()) HAS(native[0].message, "native -> sample");
 return native;
}
bool note(const logs::Triage& triage, std::string_view text) {
 return any(triage.notes, [&](auto& n) { return contains(n, text); });
}
std::set<std::string> stages(const logs::PixelChainSample& chain) {
 return projected_set(chain.stages, [](auto& item) { return j(item.first)[0].template get<std::string>(); });
}
bool has_stage(const logs::Triage& triage, std::string_view boundary) {
 return any(triage.pixel_samples, [&](auto& item) { return stages(item.second).contains(std::string(boundary)); });
}
void test_reservoir_equal_ranks_preserve_insertion_priority_after_updates() {
 logs::TriagePool pool(2);
 auto sample = row({{"event", "sample"}});
 Observe observations({"pool.ranks"});
 for (const auto& [key, rank] : std::vector<std::pair<std::string, int>>{{"a", 3}, {"b", 3}, {"a", 1}, {"b", 1}, {"c", 0}, {"late", 1}}) pool.add(sample, J::array({rank}), key, key);
 EQ(projected_set(pool.rows(), [](auto& item) { return item.key; }), (std::set<std::string>{"\"b\"", "\"c\""}));
 EQ(pool.discarded, 2U);
 for (int rank = -1; rank >= -1000; --rank) pool.add(sample, J::array({rank}), "improved", "b");
 CHECK(observations.maximum("pool.ranks") <= 4);
 EQ(project(pool.rows(), [](auto& item) { return item.why; }), (std::vector<J>{"improved", "c"}));
 EQ(pool.discarded, 2U);
}
void test_saturated_reservoir_has_logarithmic_comparison_budget() {
 constexpr std::size_t capacity = 256, total = 4096;
 logs::TriagePool pool(capacity);
 auto sample = row({{"event", "sample"}});
 Observe observations({"rank.compare", "pool.ranks"});
 for (std::size_t i = 0; i < total; ++i) pool.add(sample, J::array({total - i}), "sample", i);
 CHECK(observations.count("rank.compare") < total * 4 * std::bit_width(capacity));
 auto ranks = project(pool.rows(), [](auto& item) { return item.rank[0].template get<int>(); });
 std::vector<int> expected(capacity);
 std::iota(expected.begin(), expected.end(), 1);
 EQ(ranks, expected);
 EQ(pool.discarded, total - capacity);
 CHECK(observations.maximum("pool.ranks") <= 2 * capacity);
}
void test_counter_lookup_reads_each_field_once_at_identity_capacity() {
 auto triage = triage_fixture();
 logs::Identity strong;
 for (int i = 0; i < 64; ++i) {
  strong = {"capture", {"request_id"}, {std::to_string(i + 1)}};
  triage.add_refinement(row({{"generation", i + 1}, {"slot", 0}}, 1, std::to_string(i) + ".jsonl"), strong);
 }
 auto sample = row({{"generation", 64}, {"slot", 0}}, 1, "63.jsonl");
 Observe fields({"record.get"});
 auto neighbor = triage.counter_neighbor(sample);
 CHECK(neighbor);
 const auto reads = fields.count("record.get");
 EQ(std::get<0>(*neighbor).values, (Strings{"64", "0"}));
 EQ(std::get<1>(*neighbor), strong);
 EQ(std::get<2>(*neighbor), "line");
 CHECK(reads <= 3);
}
void test_counter_lookup_preserves_first_matching_schema_and_origin() {
 auto triage = triage_fixture();
 logs::Identity strong{"capture", {"request_id"}, {"1"}};
 auto schemas = j(R"json([{"generation":4,"slot":0},{"sequence":7,"generation":4},{"sequence":7,"generation":4,"slot":0},{"sequence":7,"slot":0}])json");
 for (std::size_t i = 0; i < schemas.size(); ++i) triage.add_refinement(row(schemas[i], i + 1), strong);
 auto neighbor = triage.counter_neighbor(row(schemas[2], 20));
 CHECK(neighbor);
 EQ(std::get<0>(*neighbor).names, (Strings{"generation", "slot"}));
 FALSE(triage.counter_neighbor(row(schemas[2], 1000)));
 FALSE(triage.counter_neighbor(row({{"generation", "4"}, {"slot", false}})));
}
void test_automatic_anchor_search_is_logarithmic_and_returns_two_neighbors() {
 logs::AutoAnchorGroup group;
 for (int i = 0; i < logs::MAX_AUTO_ANCHORS; ++i) group.add(i, row({{"steady_ns", i}}, i + 1));
 group.finish();
 Observe times({"anchor.read"});
 EQ(group.nearest(row({{"steady_ns", 64}})), (std::vector<std::size_t>{63, 64}));
 CHECK(times.count("anchor.read") <= std::bit_width(static_cast<unsigned>(logs::MAX_AUTO_ANCHORS)) + 2);
 EQ(group.nearest(row(J::object(), 1, "another.log")), std::vector<std::size_t>{0});
}
void test_pixel_edges_keep_the_same_evidence_for_every_arrival_order() {
 permutations({"native", "forwarded", "import", "mailbox", "sample"}, [](auto& order) {
  auto triage = triage_fixture();
  std::size_t line = 0;
  for (auto& boundary : order) triage.observe_pixel_chain(pixel_row(boundary, 1, boundary == "native" ? 10 : 11, ++line), true);
  triage.finalize_pixel_chain(true);
  auto found = findings(triage);
  auto& finding = only(found);
  EQ(finding.kind, "pixel-chain-divergence");
  HAS(finding.message, "native -> import");
  EQ(finding.record.get("boundary"), J("import"));
  CHECK(finding.other);
  EQ(finding.other->get("boundary"), J("native"));
  EQ(triage.pixel_pending_count, 0U);
 });
}
void test_direct_pixels_join_only_the_acquired_source_in_every_arrival_order() {
 permutations({"native", "forwarded", "sample"}, [](auto& order) {
  for (int rgba : {10, 11}) {
   auto triage = triage_fixture(false);
   for (auto& boundary : order) {
    auto sample = pixel_row(boundary, 1, boundary == "sample" ? rgba : 10);
    sample.data["direct_sampling"] = true;
    triage.observe_pixel_chain(sample, true);
   }
   triage.finalize_pixel_chain(true);
   auto found = findings(triage);
   if (rgba == 10)
    FALSE(found);
   else {
    auto& finding = only(found);
    EQ(finding.kind, "pixel-chain-divergence");
    HAS(finding.message, "native -> sample");
   }
  }
 });
}
void test_direct_pixels_reject_copy_receipts_before_or_after_acquisition() {
 permutations({"forwarded", "import", "sample"}, [](auto& order) {
  auto triage = triage_fixture(false);
  for (auto& boundary : order) {
   auto sample = pixel_row(boundary);
   sample.data["direct_sampling"] = true;
   triage.observe_pixel_chain(sample, true);
  }
  auto found = findings(triage);
  EQ(only(found).kind, "pixel-mode-conflict");
 });
}
void test_optional_direct_image_edges_preserve_native_comparison_in_every_arrival_order() {
 permutations({"native", "forwarded", "direct_image", "sample"}, [](auto& order) {
  for (auto [image, sampled] : std::vector<std::pair<int, int>>{{10, 10}, {10, 11}, {11, 11}, {11, 10}}) {
   auto triage = triage_fixture(false);
   std::size_t line = 0;
   for (auto& boundary : order) {
    auto sample = pixel_row(boundary, 1, boundary == "direct_image" ? image : boundary == "sample" ? sampled : 10, ++line);
    sample.data["direct_sampling"] = true;
    triage.observe_pixel_chain(sample, true);
   }
   triage.finalize_pixel_chain(true);
   auto native = expect_native_pixel_comparison(triage, sampled != 10);
   auto optional = findings(triage, "pixel-direct-boundary-divergence");
   EQ(optional.size(), static_cast<std::size_t>((image != 10) + (image != sampled)));
   if (image != 10) CHECK(any(optional, [](auto& f) { return contains(f.message, "native -> direct_image"); }));
   if (image != sampled) CHECK(any(optional, [](auto& f) { return contains(f.message, "direct_image -> sample"); }));
   EQ(triage.findings.size(), native.size() + optional.size());
  }
 });
}
void test_optional_direct_image_requires_completion_marker_and_exact_direct_bridge() {
 for (const auto& defect :
  j(R"json([{"completion":"Ok(false)"},{"readback_marker":0},{"expected_marker":0},{"readback_marker":0,"expected_marker":0},{"completion":null},{"expected_marker":null}])json")) {
  auto triage = triage_fixture(false);
  int line = 0;
  for (auto boundary : {"native", "forwarded", "direct_image", "sample"}) {
   auto sample = pixel_row(boundary, 1, std::string_view(boundary) == "native" ? 10 : 11, ++line);
   sample.data["direct_sampling"] = true;
   if (std::string_view(boundary) == "direct_image") sample.data.update(defect);
   triage.observe_pixel_chain(sample, true);
  }
  triage.finalize_pixel_chain(true);
  auto found = findings(triage);
  EQ(only(found).kind, "pixel-chain-divergence");
  HAS(found[0].message, "native -> sample");
  CHECK(note(triage, "readback marker"));
 }
 for (bool bridge : {false, true}) {
  auto triage = triage_fixture(false);
  triage.observe_pixel_chain(pixel_row("direct_image", 1, 11), true);
  if (bridge) {
   auto sample = pixel_row("forwarded", 1, 10, 2);
   sample.data["direct_sampling"] = false;
   triage.observe_pixel_chain(sample, true);
  }
  triage.finalize_pixel_chain(true);
  FALSE(triage.findings);
  FALSE(triage.pixel_pending);
  CHECK(note(triage, "bytes were not compared"));
 }
}
void test_complete_source_copy_pixels_compare_exact_edges_in_every_group_order() {
 const std::map<std::string, Strings> groups = {
  {"native", {"native", "native_after_release"}}, {"source_copies", {"source_copy_before_ready", "source_copy_after_release"}}, {"browser", {"direct_image", "sample"}}, {"forwarded", {"forwarded"}}
 };
 const std::map<std::string, std::vector<std::pair<std::string, std::string>>> edges = {
  {"pixel-chain-divergence", {{"native", "sample"}}}, {"pixel-direct-boundary-divergence", {{"native", "direct_image"}, {"direct_image", "sample"}}},
  {"pixel-source-copy-boundary-divergence",
   {{"native", "source_copy_before_ready"}, {"native_after_release", "source_copy_after_release"}, {"source_copy_before_ready", "source_copy_after_release"},
    {"source_copy_before_ready", "direct_image"}, {"source_copy_after_release", "direct_image"}, {"source_copy_before_ready", "sample"}, {"source_copy_after_release", "sample"}}}
 };
 permutations({"native", "source_copies", "browser", "forwarded"}, [&](auto& order) {
  for (const auto& changed : std::vector<std::set<std::string>>{
        {}, {"source_copy_before_ready", "source_copy_after_release"}, {"source_copy_after_release"}, {"direct_image", "sample"}, {"direct_image"}, {"native_after_release"}
       }) {
   auto triage = triage_fixture(false);
   std::size_t line = 0;
   for (auto& group : order)
    for (auto& boundary : groups.at(group))
     for (int index = 0; index < (boundary == "forwarded" ? 1 : 25); ++index) {
      auto sample = pixel_row(boundary, 1, changed.contains(boundary) ? 11 : 10, ++line, index);
      sample.data.update({{"direct_sampling", true}, {"workspace_allocation", 57}, {"timeline_ready", 1}});
      triage.observe_pixel_chain(sample, true);
     }
   triage.finalize_pixel_chain(true);
   for (const auto& [kind, pairs] : edges) {
    auto observed = projected_set(findings(triage, kind), [](auto& f) { return f.message.substr(0, f.message.find(" sample differs")); });
    std::set<std::string> expected;
    for (const auto& [left, right] : pairs)
     if (changed.contains(left) != changed.contains(right)) expected.insert(left + " -> " + right);
    EQ(observed, expected);
   }
   CHECK(all(findings(triage), [](auto& f) { return f.kind.ends_with("divergence"); }));
   EQ(triage.findings.size(), triage.pixel_divergences.size());
   EQ(triage.pixel_samples.size(), 25U);
   CHECK(all(triage.pixel_samples, [](auto& item) { return item.second.stages.size() == 6; }));
   EQ(triage.pixel_pending_count, 0U);
  }
 });
}
void test_optional_source_copy_absence_and_retired_alias_keep_required_native_evidence() {
 for (const auto& optional : std::vector<Strings>{{}, {"source_copy_before_ready"}, {"source_copy_after_release"}, {"source_copy_before_ready", "source_copy_after_release"}})
  for (int rgba : {10, 11}) {
   auto triage = triage_fixture(false);
   auto boundaries = optional;
   boundaries.insert(boundaries.end(), {"native", "forwarded", "sample"});
   std::size_t line = 0;
   for (auto& boundary : boundaries) {
    auto sample = pixel_row(boundary, 1, boundary == "native" ? 10 : rgba, ++line);
    sample.data["direct_sampling"] = true;
    triage.observe_pixel_chain(sample, true);
   }
   auto alias = pixel_row("direct_image", 1, 99, 10);
   alias.data["event"] = "firefox.workspace.direct_memory_pixel";
   triage.observe_pixel_chain(alias, true);
   triage.finalize_pixel_chain(true);
   expect_native_pixel_comparison(triage, rgba != 10);
   if (rgba == 10) FALSE(triage.findings);
   FALSE(any(findings(triage), [](auto& f) { return contains(f.message, "direct_image"); }));
   CHECK(note(triage, "excluded from pixel comparisons"));
  }
}
void test_optional_native_pixels_without_a_bridge_never_create_missing_required_evidence() {
 for (auto boundary : {"source_copy_before_ready", "source_copy_after_release", "native_after_release"})
  for (bool emit : {false, true}) {
   auto triage = triage_fixture(false);
   triage.observe_pixel_chain(pixel_row(boundary), emit);
   triage.finalize_pixel_chain(emit);
   FALSE(triage.findings);
   EQ(triage.seed_pool.size(), 0U);
   FALSE(triage.pixel_samples);
   EQ(triage.pixel_pending_count, 0U);
   CHECK(note(triage, "bytes were not compared"));
   triage.observe_pixel_chain(pixel_row("forwarded", 1, 10, 2), emit);
   triage.observe_pixel_chain(pixel_row(boundary, 1, 10, 3), emit);
   EQ(stages(only(triage.pixel_samples).second), std::set<std::string>{boundary});
   FALSE(triage.findings);
  }
}
void test_source_copy_pixels_reject_conflicting_bridge_facts_in_either_order() {
 for (auto boundary : {"source_copy_before_ready", "source_copy_after_release"})
  for (const auto& defect : j(R"json([{"source":"00000000000000050000000000000006"},{"workspace_allocation":58},{"timeline_ready":3},{"timeline_ready":true}])json"))
   for (const auto& order : std::vector<Strings>{{"forwarded", boundary}, {boundary, "forwarded"}}) {
    auto triage = triage_fixture(false);
    Strings sequence = {"native", "sample"};
    sequence.insert(sequence.end(), order.begin(), order.end());
    int line = 0;
    for (auto& stage : sequence) {
     auto sample = pixel_row(stage, 1, stage == boundary ? 99 : 10, ++line);
     sample.data.update({{"direct_sampling", true}, {"workspace_allocation", 57}, {"timeline_ready", 1}});
     if (stage == boundary) sample.data.update(defect);
     triage.observe_pixel_chain(sample, true);
    }
    triage.finalize_pixel_chain(true);
    auto found = findings(triage);
    EQ(only(found).kind, "pixel-source-copy-transfer-conflict");
    HAS(found[0].message, "exact forwarded transfer");
    FALSE(has_stage(triage, boundary));
   }
}
void test_source_copy_pixels_never_join_another_transfer_publication_surface_or_run() {
 for (auto boundary : {"source_copy_before_ready", "source_copy_after_release"})
  for (const auto& [field, value] :
   std::vector<std::pair<std::string, J>>{{"transfer_sequence", 2}, {"presentation_revision", 2}, {"surface", "00000000000000070000000000000008"}, {"@run", "another"}}) {
   auto triage = triage_fixture(false);
   int line = 0;
   for (auto stage : {boundary, "native", "forwarded", "sample"}) {
    auto sample = pixel_row(stage, 1, std::string_view(stage) == boundary ? 99 : 10, ++line);
    sample.data["direct_sampling"] = true;
    if (std::string_view(stage) == boundary) {
     if (field == "@run")
      sample.metadata["run"] = value;
     else
      sample.data[field] = value;
    }
    triage.observe_pixel_chain(sample, true);
   }
   triage.finalize_pixel_chain(true);
   FALSE(triage.findings);
   EQ(triage.pixel_pending_count, 0U);
   FALSE(has_stage(triage, boundary));
  }
}
void test_source_copy_edges_reject_contradictory_coordinates_and_iced_source_facts() {
 for (const auto& defect :
  j(
   R"json([{"sample_x":9},{"sample_y":9},{"source":"00000000000000050000000000000006"},{"transfer_sequence":2},{"transfer_sequence":true},{"workspace_allocation":58},{"workspace_allocation":57.0}])json")) {
  auto triage = triage_fixture(false);
  int line = 0;
  for (auto stage : {"source_copy_before_ready", "source_copy_after_release", "native", "forwarded", "sample"}) {
   auto sample = pixel_row(stage, 1, 10, ++line);
   sample.data.update({{"direct_sampling", true}, {"workspace_allocation", 57}});
   if (std::string_view(stage) == "sample") sample.data.update(defect);
   triage.observe_pixel_chain(sample, true);
  }
  triage.finalize_pixel_chain(true);
  auto optional = select(findings(triage), [](auto& f) { return f.kind.starts_with("pixel-source-copy"); });
  EQ(optional.size(), 2U);
  CHECK(all(optional, [](auto& f) { return f.kind == "pixel-source-copy-transfer-conflict" && contains(f.message, "bytes were not compared"); }));
 }
}
void test_source_copy_pixels_with_ambiguous_native_source_spelling_are_not_compared() {
 for (auto source : {"invalid", "00000000000000050000000000000006"}) {
  auto triage = triage_fixture(false);
  int line = 0;
  for (auto stage : {"native", "forwarded", "source_copy_before_ready", "sample"}) {
   auto sample = pixel_row(stage, 1, std::string_view(stage) == "source_copy_before_ready" ? 99 : 10, ++line);
   sample.data["direct_sampling"] = true;
   if (std::string_view(stage) == "source_copy_before_ready") sample.data.update({{"source", source}, {"workspace_source_high", 3}, {"workspace_source_low", 4}});
   triage.observe_pixel_chain(sample, true);
  }
  triage.finalize_pixel_chain(true);
  FALSE(triage.findings);
  CHECK(note(triage, "lacks an exact source identity"));
 }
}
void test_source_copy_pixels_preserve_the_required_copy_mode_chain() {
 auto triage = triage_fixture();
 int line = 0;
 for (const std::string boundary : {"native", "native_after_release", "source_copy_before_ready", "source_copy_after_release", "sample", "mailbox", "import", "forwarded"}) {
  auto sample = pixel_row(boundary, 1, boundary.starts_with("native") ? 10 : 11, ++line);
  sample.data["direct_sampling"] = false;
  triage.observe_pixel_chain(sample, true);
 }
 triage.finalize_pixel_chain(true);
 auto required = findings(triage, "pixel-chain-divergence");
 HAS(only(required).message, "native -> import");
 EQ(projected_set(findings(triage, "pixel-source-copy-boundary-divergence"), [](auto& f) { return f.message.substr(0, f.message.find(" sample differs")); }),
  (std::set<std::string>{"native -> source_copy_before_ready", "native_after_release -> source_copy_after_release"}));
 EQ(triage.findings.size(), 3U);
 EQ(triage.pixel_pending_count, 0U);
}
void test_source_copy_owner_mutation_keeps_the_first_complete_observation() {
 auto triage = triage_fixture();
 triage.observe_pixel_chain(pixel_row("forwarded"), true);
 triage.observe_pixel_chain(pixel_row("source_copy_before_ready", 1, 10, 2), true);
 triage.observe_pixel_chain(pixel_row("source_copy_before_ready", 1, 11, 3), true);
 triage.finalize_pixel_chain(true);
 auto found = findings(triage);
 auto& finding = only(found);
 EQ(finding.kind, "pixel-source-copy-owner-mutation");
 CHECK(finding.other);
 EQ(finding.other->line, 2U);
 EQ(finding.record.line, 3U);
 EQ(only(only(triage.pixel_samples).second.stages).second.sample, J::array({4, 5, 10}));
}
void test_native_pixel_timeline_retains_exact_source_copy_facts() {
 auto options = logs::parse_options({"--format", "timeline"});
 for (auto boundary : {"native", "native_after_release", "source_copy_before_ready", "source_copy_after_release"}) {
  auto sample = pixel_row(boundary, 7, 0xff332211, 1, 24);
  sample.data["workspace_allocation"] = 57;
  auto output = logs::render_record({J::array(), sample, "query", false}, options);
  for (const auto& text : Strings{
        sample.get("@event").get<std::string>(), "@workspace_source=00000000000000030000000000000004", "workspace_allocation=57", "presentation_revision=1", "transfer_sequence=7", "sample_index=24",
        "sample_x=8", "sample_y=9", "sample_rgba=4281541137"
       })
   HAS(output, text);
 }
}
void test_bounded_failure_payload_preserves_reason_and_identities_before_large_context() {
 auto sample = row(
  {{"audit_context", {{"prior", {{"visible_indices", array(200)}}}, {"field", "rows"}, {"expected", 4}}}, {"event", "acceptance.atlas_draw.failed"}, {"observed_event", "iced.surface.draw_encoded"},
   {"reason", "draw_source_facts_mismatch"}, {"source_revision", 30}, {"presentation_revision", 337}, {"surface", "00000000000000010000000000000002"}, {"source", "00000000000000030000000000000004"}});
 auto retained = logs::triage_snapshot(sample);
 for (auto field : {"@event", "reason", "observed_event", "source_revision", "presentation_revision", "@surface", "@workspace_source", "audit_context.field", "audit_context.expected"})
  EQ(retained.get(field), sample.get(field));
 CHECK(retained.metadata["triage_payload_truncated"]);
}
void test_absent_pixel_bridges_finalize_each_exact_transfer_once() {
 auto triage = triage_fixture();
 for (auto [boundary, transfer, line] : std::vector<std::tuple<std::string, int, int>>{{"import", 7, 9}, {"mailbox", 7, 3}, {"import", 8, 4}})
  triage.observe_pixel_chain(pixel_row(boundary, transfer, 10, line), true);
 EQ(triage.pixel_pending_count, 3U);
 FALSE(triage.findings);
 triage.finalize_pixel_chain(true);
 auto found = findings(triage);
 std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.record.line < b.record.line; });
 EQ(project(found, [](auto& f) { return f.kind; }), (Strings{"pixel-transfer-missing", "pixel-transfer-missing"}));
 EQ(project(found, [](auto& f) { return f.record.line; }), (std::vector<std::size_t>{3, 4}));
 for (std::size_t i = 0; i < found.size(); ++i) {
  HAS(found[i].message, "run=capture");
  HAS(found[i].message, "surface=00000000000000010000000000000002");
  HAS(found[i].message, "presentation_revision=1 transfer_sequence=" + std::to_string(i + 7));
  CHECK(logs::missing(found[i].record.get("@workspace_source")));
 }
 HAS(found[0].message, "2 pixel receipt(s)");
 expect_no_pending_pixels(triage);
 triage.finalize_pixel_chain(true);
 EQ(triage.findings.size(), 2U);
}
void test_exact_pixel_bridge_succeeds_before_or_after_the_pixel() {
 for (const auto& order : std::vector<Strings>{{"import", "forwarded"}, {"forwarded", "import"}}) {
  auto triage = triage_fixture(false);
  triage.observe_pixel_chain(pixel_row("native"), true);
  for (auto& boundary : order) triage.observe_pixel_chain(pixel_row(boundary), true);
  triage.finalize_pixel_chain(true);
  FALSE(triage.findings);
  expect_no_pending_pixels(triage);
  EQ(stages(only(triage.pixel_samples).second), (std::set<std::string>{"native", "import"}));
 }
}
void test_pending_pixel_capacity_counts_replay_and_finalization_exactly() {
 auto triage = triage_fixture();
 Limit limit(mmltk::log_query::detail::limits.pixel_samples, 3);
 int line = 0;
 for (int transfer : {1, 1, 2, 3}) triage.observe_pixel_chain(pixel_row("import", transfer, 10, ++line), true);
 EQ(triage.pixel_pending_count, 3U);
 std::size_t pending = 0;
 for (auto& [key, values] : triage.pixel_pending) pending += values.size();
 EQ(pending, 3U);
 CHECK(note(triage, "Unbridged pixel sample capacity reached"));
 triage.observe_pixel_chain(pixel_row("forwarded", 1, 10, 5), true);
 EQ(triage.pixel_pending_count, 1U);
 triage.observe_pixel_chain(pixel_row("import", 3, 10, 6), true);
 EQ(triage.pixel_pending_count, 2U);
 triage.finalize_pixel_chain(true);
 EQ(triage.findings.size(), 2U);
 expect_no_pending_pixels(triage);
}
void test_saturated_bridge_index_reports_incomplete_for_later_pixels() {
 for (bool emit : {false, true}) {
  PixelTriageFixture fixture(2, false);
  auto& triage = fixture.triage;
  for (int transfer : {1, 2, 3}) triage.observe_pixel_chain(pixel_row("forwarded", transfer), emit);
  EQ(triage.pixel_sources.size(), 2U);
  CHECK(triage.pixel_sources_truncated);
  int line = 0;
  for (auto boundary : {"import", "mailbox"}) triage.observe_pixel_chain(pixel_row(boundary, 3, 10, ++line), emit);
  EQ(triage.pixel_pending_count, 2U);
  triage.finalize_pixel_chain(emit);
  expect_no_pending_pixels(triage);
  FALSE(triage.pixel_samples);
  CHECK(note(triage, "Pixel transfer bridge capacity reached"));
  if (emit) {
   auto found = findings(triage);
   EQ(only(found).kind, "pixel-transfer-incomplete");
   for (auto text : {"bridge index was truncated", "transfer_sequence=3", "2 pixel receipt(s)"}) HAS(found[0].message, text);
  } else
   HAS(triage.seed_pool.rows()[0].why, "pixel-transfer-incomplete");
  auto before = std::pair{triage.findings.size(), triage.seed_pool.size()};
  triage.finalize_pixel_chain(emit);
  EQ((std::pair{triage.findings.size(), triage.seed_pool.size()}), before);
  EQ(triage.pixel_pending_count, 0U);
 }
}
void test_saturated_bridge_index_resolves_pending_pixels_directly() {
 for (int rgba : {10, 11}) {
  PixelTriageFixture fixture(2, false);
  auto& triage = fixture.triage;
  for (int transfer : {1, 2}) triage.observe_pixel_chain(pixel_row("forwarded", transfer), true);
  triage.observe_pixel_chain(pixel_row("native", 3), true);
  for (auto boundary : {"import", "mailbox"}) triage.observe_pixel_chain(pixel_row(boundary, 3, rgba), true);
  EQ(triage.pixel_pending_count, 2U);
  triage.observe_pixel_chain(pixel_row("forwarded", 3), true);
  EQ(triage.pixel_sources.size(), 2U);
  CHECK(triage.pixel_sources_truncated);
  expect_no_pending_pixels(triage);
  triage.finalize_pixel_chain(true);
  EQ(stages(only(triage.pixel_samples).second), (std::set<std::string>{"native", "import", "mailbox"}));
  EQ(projected_set(findings(triage), [](auto& f) { return f.kind; }), rgba != 10 ? std::set<std::string>{"pixel-chain-divergence"} : std::set<std::string>{});
  CHECK(note(triage, "Pixel transfer bridge capacity reached"));
 }
}
void test_saturated_bridge_direct_replay_preserves_corroborating_conflict() {
 auto triage = triage_fixture();
 Limit limit(mmltk::log_query::detail::limits.pixel_samples, 1);
 triage.observe_pixel_chain(pixel_row("forwarded"), true);
 auto pixel = pixel_row("import", 2);
 pixel.data["generation"] = 4;
 triage.observe_pixel_chain(pixel, true);
 auto bridge = pixel_row("forwarded", 2);
 bridge.data["generation"] = 5;
 triage.observe_pixel_chain(bridge, true);
 triage.finalize_pixel_chain(true);
 auto found = findings(triage);
 EQ(only(found).kind, "pixel-transfer-conflict");
 FALSE(triage.pixel_samples);
 EQ(triage.pixel_pending_count, 0U);
}
void test_full_but_untruncated_bridge_index_preserves_proven_missing() {
 auto triage = triage_fixture();
 Limit limit(mmltk::log_query::detail::limits.pixel_samples, 2);
 for (int transfer : {1, 2}) triage.observe_pixel_chain(pixel_row("forwarded", transfer), true);
 triage.observe_pixel_chain(pixel_row("import", 3), true);
 triage.finalize_pixel_chain(true);
 FALSE(triage.pixel_sources_truncated);
 auto found = findings(triage);
 EQ(only(found).kind, "pixel-transfer-missing");
 EQ(triage.pixel_pending_count, 0U);
}
void test_missing_pixel_finalization_respects_finding_and_anchor_budgets() {
 for (bool emit : {false, true}) {
  auto triage = triage_fixture(false);
  {
   Limit limit(mmltk::log_query::detail::limits.pixel_samples, 70);
   for (int transfer = 1; transfer < 72; ++transfer) triage.observe_pixel_chain(pixel_row("import", transfer, 10, transfer), emit);
   EQ(triage.pixel_pending_count, 70U);
   triage.finalize_pixel_chain(emit);
  }
  expect_no_pending_pixels(triage);
  if (emit) {
   EQ(triage.findings.size(), 64U);
   CHECK(note(triage, "Finding inventory capped"));
  } else {
   FALSE(triage.findings);
   EQ(triage.seed_pool.size(), static_cast<std::size_t>(std::max(32, triage.options.triage_anchors * 4)));
   CHECK(triage.seed_pool.discarded > 0);
  }
 }
}
void test_source_less_pixel_requires_an_exact_forwarded_transfer() {
 auto triage = triage_fixture();
 triage.observe_pixel_chain(pixel_row("native"), true);
 triage.observe_pixel_chain(pixel_row("import", 1, 11), true);
 triage.observe_pixel_chain(pixel_row("forwarded", 2), true);
 FALSE(triage.findings);
 triage.observe_pixel_chain(pixel_row("forwarded"), true);
 EQ(triage.findings.begin()->second.kind, "pixel-chain-divergence");
}
void test_forwarded_source_conflict_is_rejected() {
 auto triage = triage_fixture();
 triage.observe_pixel_chain(pixel_row("forwarded"), true);
 auto conflicting = pixel_row("forwarded", 1, 10, 2);
 conflicting.data["source"] = "00000000000000050000000000000006";
 triage.observe_pixel_chain(conflicting, true);
 triage.finalize_pixel_chain(true);
 EQ(triage.findings.begin()->second.kind, "pixel-transfer-conflict");
}
void test_pixel_owner_mutation_wins_before_cross_stage_comparison() {
 auto triage = triage_fixture();
 triage.observe_pixel_chain(pixel_row("native"), true);
 triage.observe_pixel_chain(pixel_row("native", 1, 11, 2), true);
 triage.observe_pixel_chain(pixel_row("import", 1, 12, 3), true);
 auto found = findings(triage);
 auto& finding = only(found);
 EQ(finding.kind, "pixel-owner-mutation");
 CHECK(finding.other);
 EQ(finding.other->line, 1U);
 EQ(finding.record.line, 2U);
}
void test_pixel_transfer_comparisons_are_linear_in_samples() {
 auto triage = triage_fixture();
 triage.observe_pixel_chain(pixel_row("sample"), true);
 EQ(triage.pixel_samples.size(), 1U);
 auto& chain = triage.pixel_samples.begin()->second;
 Observe observations({"pixel.stage"});
 constexpr int transfers = 256;
 for (int transfer = 1; transfer <= transfers; ++transfer)
  for (auto boundary : {"native", "forwarded", "import", "mailbox"}) triage.observe_pixel_chain(pixel_row(boundary, transfer), true);
 std::size_t operations = 0;
 for (auto& event : observations.events)
  if (event.event == "pixel.stage") operations += event.value.get<std::size_t>();
 CHECK(operations <= transfers * 3 * 7);
 EQ(chain.mailbox_count, static_cast<std::size_t>(transfers));
 FALSE(triage.findings);
}
struct Result {
 int status;
 std::string out, err;
 auto operator<=>(const Result&) const = default;
};
struct Exported {
 int status;
 std::vector<J> rows;
 std::string err;
 bool operator==(const Exported&) const = default;
};
struct Statistics {
 int status;
 std::vector<J> values;
 std::string out, err;
};
Strings plus(Strings args, Strings extra) {
 args.insert(args.end(), extra.begin(), extra.end());
 return args;
}
struct FileFixture {
 support::TemporaryDirectory temporary{"log-query-test-"};
 fs::path root = temporary.path();
 fs::path write(std::string_view name, std::string_view contents) {
  auto path = root / name;
  fs::create_directories(path.parent_path());
  support::write_text(path, contents);
  return path;
 }
 fs::path write(std::string_view name, const char* contents) { return write(name, std::string_view(contents)); }
 fs::path write(std::string_view name, const std::string& contents) { return write(name, std::string_view(contents)); }
 fs::path write(std::string_view name, const J& contents) {
  std::string text;
  for (const auto& item : contents) text += fixture_json(item) + '\n';
  return write(name, text);
 }
 Result query(Strings args) const {
  std::ostringstream output, diagnostics;
  const int status = logs::main(args, root, output, diagnostics);
  return {status, output.str(), diagnostics.str()};
 }
 Exported exported(Strings args) const {
  auto result = query(plus(std::move(args), {"--format", "jsonl"}));
  std::vector<J> rows;
  for (const auto& line : support::lines(result.out)) rows.push_back(logs::parse_json(line));
  return {result.status, std::move(rows), result.err};
 }
 Statistics statistics(Strings args) const {
  auto result = query(std::move(args));
  std::vector<J> values;
  for (auto& line : support::lines(result.out + result.err))
   if (line.starts_with("Statistics ")) values.push_back(logs::parse_json(line.substr(11)));
  return {result.status, std::move(values), result.out, result.err};
 }
};
Rows records(const logs::LogFile& source, bool unchanged = false) {
 Rows result;
 source.records([&](auto value) { result.push_back(std::move(value)); }, unchanged);
 return result;
}
Rows records(const fs::path& path, const fs::path& root) { return records(logs::LogFile::capture(path, root)); }
std::vector<logs::LogFile> discover(const Strings& paths, const fs::path& root, bool recursive = false, const std::string& host = {}) {
 logs::Captured captured;
 std::set<fs::path> histories;
 std::map<fs::path, bool> directories;
 return logs::discover_files(paths, root, recursive, host, captured, histories, directories);
}
std::vector<J> matching_rows(const Exported& result, std::string_view reason) {
 return select(result.rows, [&](auto& r) { return r["_log"]["match"] == reason; });
}
std::set<std::string> events(const std::vector<J>& rows) {
 return projected_set(rows, [](auto& r) { return r["data"]["event"].template get<std::string>(); });
}
std::vector<J> column(const std::vector<J>& rows, std::string_view outer, std::string_view inner) {
 return project(rows, [&](auto& r) { return r[outer][inner]; });
}
std::set<J> column_set(const std::vector<J>& rows, std::string_view outer, std::string_view inner) {
 return projected_set(rows, [&](auto& r) { return r[outer][inner]; });
}
void includes(std::string_view output, const Strings& expected) {
 for (auto& text : expected) HAS(output, text);
}
void excludes(std::string_view output, const Strings& expected) {
 for (auto& text : expected) LACKS(output, text);
}
void test_json_documents_preserve_nested_facts_and_line_span() {
 FileFixture f;
 auto facts = j(R"json({"record":{"progress":{"phase":"Train","completed_images":24}},"persistence":{"degraded":false}})json");
 auto content = facts.dump(2) + "\n";
 auto path = f.write("progress.json", content);
 auto result = f.exported({path.string(), "-q", "record.progress.completed_images=24", "--strict"});
 EQ(result.status, 0);
 EQ(result.rows.size(), 1U);
 EQ(result.rows[0]["data"], facts);
 EQ(result.rows[0]["_log"]["line"], J(1));
 EQ(result.rows[0]["_log"]["line_end"], J(support::lines(content).size()));
 {
  Limit limit(mmltk::log_query::detail::limits.line_bytes, 32);
  result = f.exported({path.string(), "--strict"});
 }
 EQ(result.status, 2);
 HAS(result.rows[0]["_log"]["parse_error"], "exceeds byte limit");
}
void test_damaged_json_documents_remain_searchable() {
 FileFixture f;
 fs::path path;
 for (const std::string content : {"{\n  \"stage\": \"Dataset\"", "{\"event\":\"one\"}\n{\"event\":\"two\"}", "{\"message\":\"bad bytes \xef\xbf\xbd\"}"}) {
  path = f.write("preparation.json", content);
  auto result = f.exported({path.string()});
  EQ(result.status, 0);
  EQ(result.rows.size(), 1U);
  EQ(result.rows[0]["text"], J(content));
 }
 support::write_text(path, "{\"message\":\"bad bytes \xff\"}");
 auto result = f.exported({path.string(), "--strict"});
 EQ(result.status, 2);
 HAS(result.rows[0]["_log"]["parse_error"], "UTF-8");
}
void test_statistics_preserve_integer_sums_buckets_and_full_match_counts() {
 FileFixture f;
 const std::uint64_t start = UINT64_C(18446744073709000000);
 J values = J::array();
 for (int i = 0; i < 5; ++i) values.push_back({{"event", "cost"}, {"steady_ns", start + i}, {"bytes", UINT64_MAX}, {"duration_ns", i}});
 auto path = f.write("cost.jsonl", values);
 auto result = f.statistics({path.string(), "--stats", "bytes", "--stats", "duration_ns", "--bucket-ms", "1", "--limit", "1"});
 EQ(result.status, 0);
 auto total = select(result.values, [](auto& r) { return !r.contains("bucket_start_ns"); });
 auto bucket = select(result.values, [](auto& r) { return r.contains("bucket_start_ns"); });
 EQ(total[0]["events"], J(5));
 EQ(logs::encoded(total[0]["fields"]["bytes"]["sum"]), "92233720368547758075");
 EQ(total[0]["fields"]["duration_ns"]["mean"], J(2));
 EQ(total[0]["first_ns"], J(start));
 EQ(total[0]["last_line"], J(5));
 EQ(bucket[0]["bucket_start_ns"], J(start / 1000000 * 1000000));
 EQ(bucket[0]["events_per_second"], J(5000));
 HAS(result.out, "showing 1/5");
}
void test_statistics_separate_runs_sources_clocks_and_numeric_absence() {
 FileFixture f;
 auto path = f.write("mixed.jsonl",
  j(
   R"json([{"event":"cost","run_id":"first","steady_ns":7,"value":3},{"event":"cost","run_id":"first","steady_ns":8,"value":4},{"event":"cost","run_id":"second","steady_ns":9,"value":9},{"event":"cost","run_id":"first","elapsed_ms":1,"value":12},{"event":"cost","run_id":"first","steady_ns":10},{"event":"cost","run_id":"first","steady_ns":11,"value":true},{"event":"cost","run_id":"first","steady_ns":12,"value":"NaN"}])json"));
 auto other = f.write("other.jsonl", j(R"json([{"event":"cost","run_id":"first","steady_ns":7,"value":17}])json"));
 auto result = f.statistics({path.string(), other.string(), "--stats", "value", "--format", "jsonl", "--limit", "1"});
 EQ(result.status, 0);
 EQ(result.values.size(), 4U);
 auto selected = select(result.values, [](auto& r) { return r["recorded_run"] == "first" && r["clock"] == "steady" && r["source"].template get<std::string>().ends_with("mixed.jsonl"); });
 EQ(only(selected)["fields"]["value"], j(R"json({"count":2,"sum":7,"min":3,"max":4,"mean":"7/2","missing":1,"invalid":2})json"));
}
void test_statistics_report_capacity_and_truncated_inputs() {
 FileFixture f;
 J values = J::array();
 for (int i = 0; i < 3; ++i) values.push_back({{"event", "cost"}, {"steady_ns", i * 1000000}, {"bytes", i}});
 auto path = f.write("buckets.jsonl", values);
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 2);
  auto result = f.statistics({path.string(), "--stats", "bytes", "--bucket-ms", "1"});
  EQ(result.status, 2);
  HAS(result.err, "statistics exceed");
 }
 auto malformed = f.write("partial.jsonl", "{\"event\":\"cost\",\"bytes\":5}\n{\"event\":\"cost\",\"bytes\":NaN}\n{\"event\":");
 auto result = f.statistics({malformed.string(), "--stats", "bytes", "--strict"});
 EQ(result.status, 2);
 CHECK(any(result.values, [](auto& r) { return r["fields"]["bytes"]["sum"] == 5; }));
 HAS(logs::lower(result.err), "malformed");
}
void test_explicit_intent_correlation_connects_browser_and_native_stages() {
 FileFixture f;
 auto path = f.write("intent.jsonl",
  j(
   R"json([{"event":"integration.intent","control":"7","detail":"queued SettingsUpdate"},{"fields":{"event":"browser.intent.received","value":7}},{"fields":{"event":"browser.intent.accepted","value":7}},{"event":"integration.intent","control":"7","detail":"reply SettingsUpdate"},{"event":"browser.server.binary_received","value":7},{"event":"browser.intent.accepted","value":8}])json"));
 auto result = f.exported({path.string(), "-q", "@event=integration.intent detail:\"queued\"", "--correlate", "@intent_request", "--no-auto-correlate"});
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "line"), (std::set<J>{1, 2, 3, 4}));
}
void test_vulkan_callback_preserves_complete_text_objects_and_physical_lines() {
 FileFixture f;
 auto message = vulkan_message();
 auto path = f.write("vulkan.log", "\n" + message + "{\"event\":\"after\"}\n");
 auto rows = records(path, f.root);
 EQ(rows.size(), 2U);
 auto& sample = rows[0];
 EQ(sample.line, 2U);
 EQ(sample.get("@line_end"), J(4));
 EQ(sample.raw, support::text::trim_newlines(message));
 EQ(sample.format, "rust");
 EQ(sample.get("@event"), J("vulkan.validation"));
 EQ(sample.get("@level"), J("error"));
 EQ(sample.get("api"), J("vkCmdCopyImageToBuffer"));
 EQ(sample.get("message_id"), J("0x30009145"));
 EQ(sample.get("vk_image"), J("0x1abc"));
 EQ(sample.get("objects"), j(R"json([{"type":"IMAGE","handle":"0x1abc","name":"?"}])json"));
 CHECK(logs::missing(sample.get("browser_process_id")));
 EQ(rows[1].line, 5U);
 FALSE(sample.parse_error);
 auto result = f.exported({"vulkan.log", "-q", "has(validation_id)", "--fields", "validation_id,message,vk_image"});
 EQ(result.status, 0);
 EQ(result.rows[0]["@line_end"], J(4));
 HAS(result.rows[0]["message"], "srcImage must have transfer source usage.");
 result = f.exported({"vulkan.log", "--error", "Validation Error: [ VUID-vkCmdCopyImageToBuffer-srcImage-00186 ] Object 0"});
 EQ(result.status, 0);
 EQ(result.rows[0]["_log"]["line"], J(2));
 EQ(result.rows[0]["text"], J(sample.raw));
}
void test_vulkan_representatives_count_distinct_ids_without_shortening_messages() {
 FileFixture f;
 auto detail = repeat("complete diagnostic condition ", 100) + "If the external handle specified was created by the Vulkan API, then apply this condition.";
 auto first = vulkan_message("VUID-vkCmdCopyImageToBuffer-srcImage-00186", "0x001AbC", "IMAGE", {}, "ERROR", detail);
 auto hazard = vulkan_message("SYNC-HAZARD-WRITE-AFTER-READ", "0x001AbC", "IMAGE", {}, "WARN");
 f.write("vulkan.log", repeat(first, 50) + repeat(hazard, 3));
 Strings args = {"vulkan.log", "-q", "@event=vulkan.validation", "--group-by", "validation_id", "--representatives", "--limit", "8"};
 auto result = f.exported(args);
 EQ(result.status, 0);
 EQ(result.rows.size(), 2U);
 EQ(column(result.rows, "_log", "representative_count"), (std::vector<J>{50, 3}));
 EQ(result.rows[0]["text"], J(support::text::trim_newlines(first)));
 HAS(result.rows[0]["data"]["message"], detail);
 EQ(result.rows[1]["data"]["validation_id"], J("SYNC-HAZARD-WRITE-AFTER-READ"));
 EQ(result.rows[1]["data"]["level"], J("WARN"));
 HAS(result.err, "53 matched");
 auto timeline = f.query(plus(args, {"--format", "timeline", "--no-auto-correlate"}));
 EQ(timeline.status, 0);
 HAS(timeline.out, detail);
 HAS(timeline.out, "occurrences=50");
 for (auto incompatible : {"--tail", "--triage"}) EQ(f.query(plus(args, {incompatible})).status, 2);
}
void test_vulkan_assembly_preserves_interruptions_and_reports_bounded_fragments() {
 FileFixture f;
 auto lines = support::lines(vulkan_message());
 auto interrupted = f.write("interrupted.log", lines[0] + "\n{\"event\":\"interleaved\"}\n" + lines[1] + "\n" + lines[2] + "\n");
 auto rows = records(interrupted, f.root);
 LACKS(rows[0].raw, "transfer source usage");
 EQ(rows[1].get("@event"), J("interleaved"));
 HAS(rows[2].raw, "transfer source usage");
 EQ(rows[2].line, 3U);
 auto path = f.write("bounded.log", vulkan_message() + "{\"event\":\"after\"}\n");
 {
  Limit limit(mmltk::log_query::detail::limits.message_lines, 2);
  auto result = f.query({"bounded.log", "--strict"});
  EQ(result.status, 2);
  HAS(result.err, "continuation retained separately");
  rows = records(path, f.root);
 }
 EQ(project(rows, [](auto& r) { return r.line; }), (std::vector<std::size_t>{1, 3, 4}));
 HAS(rows[1].raw, "objects:");
 auto max_line = std::max_element(lines.begin(), lines.end(), [](auto& a, auto& b) { return a.size() < b.size(); });
 {
  Limit limit(mmltk::log_query::detail::limits.line_bytes, max_line->size() + 2);
  rows = records(path, f.root);
 }
 CHECK(any(rows, [](auto& r) { return !r.parse_error.empty(); }));
 std::string rebuilt;
 for (auto& r : rows) rebuilt += r.raw + '\n';
 EQ(rebuilt, support::read_text(path));
 auto other = support::lines(vulkan_message("SYNC-HAZARD-WRITE-AFTER-WRITE"))[1];
 f.write("mismatched.log", lines[0] + '\n' + other + '\n');
 auto result = f.exported({"mismatched.log", "-q", "@event=vulkan.validation"});
 EQ(result.rows.size(), 2U);
 NE(result.rows[0]["data"]["validation_id"], result.rows[1]["data"]["validation_id"]);
}
void test_vulkan_exact_handles_add_bounded_same_file_provenance_candidates() {
 FileFixture f;
 std::string source = "00000000000000010000000000000002";
 auto common = j(R"json({"browser_process_id":202,"vk_device":"0x10","vk_image":"0x1abc"})json");
 J values = J::array(
  {updated(common, {{"event", "firefox.workspace.image_created"}}), updated(common, {{"event", "firefox.workspace.memory_import"}, {"source", source}, {"workspace_allocation", 34}}),
   updated(common, {{"event", "firefox.workspace.memory_bound"}, {"source", source}, {"workspace_allocation", 34}, {"vk_memory", "0x30"}}),
   {{"event", "firefox.workspace.direct_binding"}, {"vk_device", "0x10"}, {"vk_image", "0x1abc"}, {"vk_memory", "0x30"}, {"source", source}, {"workspace_allocation", 34}, {"phase", "attached"}},
   {{"event", "firefox.workspace.memory_bound"}, {"browser_process_id", 202}, {"vk_memory", "0x1abc"}}});
 auto path = f.write("build/validation/vulkan-firefox.log", values);
 {
  std::ofstream out(path, std::ios::app);
  out << repeat(vulkan_message(), 100);
 }
 f.write("build/validation/vulkan-native.log", J::array({updated(common, {{"event", "firefox.workspace.image_created"}, {"browser_process_id", 303}})}));
 Strings args = {"--family", "vulkan", "-q", "@event=vulkan.validation", "--group-by", "validation_id", "--representatives", "--auto-correlate", "--limit", "10"};
 auto result = f.exported(args);
 EQ(result.status, 0);
 auto related = matching_rows(result, "auto");
 EQ(related.size(), logs::MAX_AUTO_VULKAN_RELATED);
 EQ(events(related), (std::set<std::string>{"firefox.workspace.image_created", "firefox.workspace.memory_import", "firefox.workspace.memory_bound", "firefox.workspace.direct_binding"}));
 CHECK(all(
  related, [](auto& r) { return contains(r["_log"]["auto_reason"], "same-file handle candidate; process unrecorded") && r["_log"]["file"].template get<std::string>().ends_with("-firefox.log"); }));
 EQ(select(related, [&](auto& r) { return r["data"].value("source", "") == source; }).size(), 3U);
 auto diagnostic = matching_rows(result, "query");
 FALSE(only(diagnostic)["data"].contains("browser_process_id"));
 EQ(diagnostic[0]["_log"]["representative_count"], J(100));
 auto limited = args;
 limited.back() = "2";
 EQ(f.exported(limited).rows.size(), 2U);
 auto filtered = f.exported(plus(args, {"--where", "NOT @event=firefox.workspace.memory_bound"}));
 FALSE(events(filtered.rows).contains("firefox.workspace.memory_bound"));
}
void test_vulkan_recorded_process_joins_files_in_one_run_and_rejects_other_processes() {
 FileFixture f;
 auto common = j(R"json({"vk_image":"0x1abc","vk_device":"0x10"})json");
 f.write("build/validation/vulkan-firefox.log", vulkan_message("VUID-vkCmdCopyImageToBuffer-srcImage-00186", "0x001AbC", "IMAGE", 202));
 f.write("build/validation/vulkan-native.log",
  J::array(
   {updated(common, {{"event", "firefox.workspace.image_created"}, {"browser_process_id", 202}}), updated(common, {{"event", "firefox.workspace.memory_import"}, {"browser_process_id", 303}})}));
 f.write("build/validation/vulkan-native.log.history/57-1000000000.log", J::array({updated(common, {{"event", "firefox.workspace.memory_bound"}, {"browser_process_id", 202}})}));
 auto result = f.exported({"--family", "vulkan", "--history", "-q", "@event=vulkan.validation", "--auto-correlate"});
 EQ(result.status, 0);
 auto related = matching_rows(result, "auto");
 EQ(only(related)["data"]["browser_process_id"], J(202));
 HAS(related[0]["_log"]["auto_reason"], "recorded process");
 CHECK(related[0]["_log"]["run"].get<std::string>().ends_with("@current"));
}
void test_forced_stop_joins_observed_child_records_without_reconstructing_historical_pids() {
 FileFixture f;
 auto common = j(R"json({"owner":"firefox_process","sequence":202,"pid":999})json");
 auto terminal = updated(common, {{"event", "child.signaled"}, {"value", 137}});
 f.write("build/validation/process-native.log", J::array(
                                                 {updated(common, {{"event", "child.spawned"}}), updated(common, {{"event", "stop.kill_selected"}}), terminal, updated(terminal, {{"sequence", 0}}),
                                                  updated(terminal, {{"sequence", 303}}), updated(terminal, {{"owner", "training"}})}));
 f.write("build/validation/process-native.log.history/57-1000000000.log", J::array({terminal}));
 Strings args = {"--family", "process", "--history", "-q", "@event=stop.kill_selected", "--auto-correlate"};
 auto result = f.exported(plus(args, {"--limit", "3"}));
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "line"), (std::set<J>{1, 2, 3}));
 CHECK(all(result.rows, [](auto& r) { return r["_log"]["run"].template get<std::string>().ends_with("@current"); }));
 auto related = matching_rows(result, "auto");
 EQ(related.size(), 2U);
 for (auto& r : related) HAS(r["_log"]["auto_reason"], "exact recorded process");
 for (const auto& extra : std::vector<Strings>{{"--limit", "1"}, {"--where", "@event=stop.kill_selected"}}) {
  auto bounded = f.exported(plus(args, extra));
  EQ(bounded.status, 0);
  EQ(bounded.rows.size(), 1U);
 }
}
void test_fatal_process_links_distinguish_relationships_and_reject_conflicting_facts() {
 FileFixture f;
 f.write("build/validation/process-firefox.log", firefox_fatal() + '\n');
 for (const auto& fixture :
  j(
   R"json([[{"pid":202,"pgid":202},"exact recorded process"],[{"pid":101},"recorded parent/child relationship"],[{"pid":303,"ppid":202,"pgid":202},"recorded parent/child relationship"],[{"pid":404,"ppid":999,"pgid":202},"recorded process-group membership"],[{"pid":303,"ppid":101,"pgid":202},"recorded process-group membership"],[{"pid":303,"ppid":101},null],[{"pid":404,"pgid":404},null],[{"pid":202,"pgid":404},null],[{"pid":202,"process_id":303},null],[{"pid":404,"pgid":202,"process_group_id":404},null],[{"pid":0,"ppid":0,"pgid":0},null],[{"owner":"firefox_process","event":"child.signaled","sequence":0,"pid":202},null]])json")) {
  auto candidate = updated({{"event", "firefox.workspace.channel_terminal"}}, fixture[0]);
  f.write("build/validation/process-native.log", J::array({candidate}));
  f.write("build/validation/process-native.log.history/57-1000000000.log", J::array({candidate}));
  auto result = f.exported({"--family", "process", "--history", "-q", "@event=fatal.report", "--auto-correlate"});
  EQ(result.status, 0);
  EQ(result.rows.size(), fixture[1].is_null() ? 1U : 2U);
  auto related = matching_rows(result, "auto");
  if (fixture[1].is_null()) {
   FALSE(related);
   continue;
  }
  EQ(only(related)["data"], candidate);
  CHECK(related[0]["_log"]["run"].get<std::string>().ends_with("@current"));
  HAS(related[0]["_log"]["auto_reason"], fixture[1].get<std::string>());
  if (fixture[1] == "recorded process-group membership") HAS(related[0]["_log"]["auto_reason"], "not a crash-cause attribution");
 }
}
void test_fatal_envelopes_use_the_reader_byte_bound_and_keep_status_opaque() {
 FileFixture f;
 auto prefix = firefox_fatal("firefox.webgpu", "signal_retire", 202, 101, 202, "actor=7 device=9 submission=11 ", "");
 auto status = padded(R"json({"pid":999,"pgid":888} pid=777 trace_id=42 )json", 2044 - prefix.size(), 'x') + "...";
 auto firefox = prefix + status;
 auto native = "fatal: " + std::string(160, 'n') + "...: " + repeat("detail ", 100) + "... (status=75)";
 auto path = f.write("fatal.log", native + '\n' + firefox + '\n');
 auto rows = records(path, f.root);
 EQ(rows.size(), 2U);
 EQ(rows[0].get("status"), J("75"));
 EQ(rows[1].get("status"), J(status));
 EQ(project(rows, [](auto& r) { return r.raw; }), (Strings{native, firefox}));
 for (auto& r : rows) {
  EQ(r.get("@event"), J("fatal.report"));
  CHECK(r.get("@error"));
  FALSE(r.parse_error);
  CHECK(logs::missing(r.get("trace_id")));
 }
 {
  Limit limit(mmltk::log_query::detail::limits.line_bytes, 1024);
  rows = records(path, f.root);
  auto result = f.query({"fatal.log", "--strict"});
  EQ(result.status, 2);
  HAS(result.err, "truncated");
 }
 FALSE(rows[0].parse_error);
 CHECK(rows[1].parse_error);
 EQ(rows[1].raw, firefox.substr(0, 1024));
 EQ(rows[1].get("@event"), J("fatal.report"));
 EQ(rows[1].get("status"), J(status.substr(0, 1024 - prefix.size())));
 EQ(rows[1].get("@process"), J(202));
 CHECK(logs::missing(rows[1].get("trace_id")));
}
void test_vulkan_ambiguous_handle_owners_are_reported_without_asserted_provenance() {
 FileFixture f;
 auto common =
  j(R"json({"event":"firefox.workspace.memory_import","browser_process_id":202,"vk_device":"0x10","vk_image":"0x1abc","workspace_allocation":34,"source":"00000000000000010000000000000002"})json");
 for (const auto& conflict : j(R"json([{"browser_process_id":303},{"vk_device":"0x11"},{"workspace_allocation":35},{"source":"00000000000000010000000000000003"}])json")) {
  f.write("vulkan.log", fixture_json(common) + '\n' + vulkan_message() + fixture_json(updated(common, conflict)) + '\n');
  auto result = f.exported({"vulkan.log", "-q", "@event=vulkan.validation", "--auto-correlate"});
  EQ(result.status, 0);
  EQ(result.rows.size(), 1U);
  HAS(result.err, "conflicting recorded process/device/source/allocation");
 }
}
Strings automatic_fixture(FileFixture& f, int progress = 0) {
 f.write("build/validation/auto.jsonl",
  j(
   R"json([{"event":"explore.frame.published","owner":"explore","source_session":1,"source_instance":1,"source_revision":7,"steady_ns":100},{"event":"presentation.frame.edge","owner":"presentation","presentation_revision":11,"source_session":1,"source_instance":1,"source_revision":7,"steady_ns":110},{"event":"presentation.pixel","presentation_revision":11,"steady_ns":111}])json"));
 J rows = J::array();
 for (int i = 0; i < progress; ++i)
  rows.push_back({{"event", "integration.phase_progress"}, {"control", "work"}, {"detail", "AwaitPhase" + std::to_string(i)}, {"elapsed_ms", i}, {"a", 0}, {"b", 0}, {"c", 0}, {"d", 0}});
 for (auto& r :
  j(
   R"json([{"event":"integration.explore_state","control":"explore.gallery","a":19,"elapsed_ms":1000},{"event":"integration.explore_reopen_wait","control":"explore.open","detail":"physical-gallery-draw","a":19,"b":7,"c":3,"d":4,"elapsed_ms":1001},{"event":"integration.explore_reopen_draw","control":"explore.gallery","detail":"physical-gallery-draw","a":12,"b":6,"c":7,"d":11,"elapsed_ms":1002}])json"))
  rows.push_back(r);
 f.write("build/validation/auto-firefox.log", rows);
 return {
  "--family", "auto", "--run", "current", "-q",
  "event=integration.phase_progress OR event=integration.explore_reopen_wait OR event=integration.explore_reopen_draw OR event=integration.explore_reopened"
 };
}
void test_automatic_timeline_interleaves_exact_frame_snapshot_and_publication() {
 FileFixture f;
 auto args = automatic_fixture(f);
 auto result = f.query(plus(args, {"--format", "timeline", "--fields", "event,control,a,b,c,d,detail"}));
 EQ(result.status, 0);
 auto lines = support::lines(result.out);
 EQ(select(lines, [](auto& l) { return contains(l, "[query]"); }).size(), 2U);
 EQ(select(lines, [](auto& l) { return contains(l, "[auto:"); }).size(), 3U);
 CHECK(result.out.find("explore.frame.published") < result.out.find("integration.explore_reopen_wait"));
 CHECK(result.out.find("presentation.frame.edge") < result.out.find("integration.explore_reopen_draw"));
 includes(result.out, {"explore.frame=7", "explore.snapshot=19", "presentation_revision=11"});
 LACKS(result.out, "presentation.pixel");
 HAS(result.err, "cross-clock times remain unaligned");
}
void test_upscale_settlement_joins_native_frontier_without_inventing_presentation_match() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"upscale.frame.published","owner":"upscale","source_revision":30},{"event":"presentation.frame.edge","presentation_revision":336,"source_revision":28},{"event":"integration.upscale_settlement","control":"explore.detail.upscale.basic","detail":"blocked=sampleable_presentation_mismatch","a":"104","b":"30","c":"336","d":"337"}])json"));
 auto result = f.exported({"input.jsonl", "-q", "@event=integration.upscale_settlement", "--auto-correlate"});
 EQ(events(result.rows), (std::set<std::string>{"integration.upscale_settlement", "upscale.frame.published"}));
}
void test_timeline_failure_highlights_show_reason_original_event_and_provenance() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"acceptance.atlas_draw.failed","reason":"draw_source_facts_mismatch","observed_event":"iced.surface.draw_encoded","observed_file":"firefox.log","observed_line":82,"audit_context":{"field":"rows","expected":4},"rows":3}])json"));
 auto result = f.query({"input.jsonl", "-q", "@event=acceptance.atlas_draw.failed", "--format", "timeline"});
 EQ(result.status, 0);
 includes(result.out, {"draw_source_facts_mismatch", "iced.surface.draw_encoded", "firefox.log", "observed_line=82", "audit_context.field=rows"});
}
void test_automatic_machine_formats_require_opt_in_and_preserve_query_rows() {
 FileFixture f;
 auto args = automatic_fixture(f);
 auto baseline = f.exported(args);
 EQ(f.exported(plus(args, {"--no-auto-correlate"})), baseline);
 EQ(baseline.rows.size(), 2U);
 auto result = f.exported(plus(args, {"--auto-correlate"}));
 EQ(result.status, 0);
 EQ(matching_rows(result, "query"), baseline.rows);
 auto automatic = matching_rows(result, "auto");
 EQ(automatic.size(), 3U);
 CHECK(all(automatic, [](auto& r) { return truth(r["_log"]["auto_reason"]) && truth(r["_log"]["auto_anchor"]); }));
 auto projected = f.exported(plus(args, {"--auto-correlate", "--fields", "event"}));
 CHECK(all(projected.rows, [](auto& r) { return r["@match"] != "auto" || (truth(r["@auto_reason"]) && truth(r["@auto_anchor"])); }));
 LACKS(f.query(args).out, "[auto:");
 LACKS(f.query(plus(args, {"--format", "timeline", "--no-auto-correlate"})).out, "[auto:");
}
void test_automatic_recent_highlight_keeps_final_reopen_visible_after_phase_volume() {
 FileFixture f;
 auto args = automatic_fixture(f, 201);
 auto result = f.query(plus(args, {"--format", "timeline", "--limit", "350", "--fields", "event,control,a,b,c,d,detail"}));
 EQ(result.status, 0);
 CHECK(result.out.starts_with("Recent query matches"));
 auto first = result.out.find("@file=");
 CHECK(first != std::string::npos);
 includes(result.out.substr(0, first), {"integration.explore_reopen_wait", "integration.explore_reopen_draw"});
 LACKS(result.out, "integration.explore_reopened");
 EQ(occurrences(result.out, "[query]"), 203U);
 includes(result.out.substr(first), {"explore.frame.published", "presentation.frame.edge"});
 HAS(result.err, "75 query anchors outside");
 auto exported = f.exported(plus(args, {"--auto-correlate", "--limit", "350"}));
 EQ(exported.status, 0);
 auto selected = matching_rows(exported, "query");
 EQ(selected.size(), 203U);
 auto expected = array(201);
 expected.push_back(1001);
 expected.push_back(1002);
 EQ(J(column(selected, "data", "elapsed_ms")), expected);
 auto limited = f.query(plus(args, {"--format", "timeline", "--limit", "20", "--fields", "event"}));
 EQ(occurrences(limited.out, "[query]"), 20U);
 includes(limited.out.substr(0, limited.out.find("@file=")), {"integration.explore_reopen_wait", "integration.explore_reopen_draw"});
 std::string text;
 for (int i = 0; i < 30; ++i) text += "ordinary message " + std::to_string(i) + '\n';
 f.write("plain.log", text);
 auto plain = f.query({"plain.log", "-q", "ordinary", "--format", "timeline", "--limit", "2"});
 EQ(plain.status, 0);
 HAS(plain.out.substr(0, plain.out.find("[query]")), "ordinary message 29");
 EQ(occurrences(plain.out, "[query]"), 2U);
}
void test_automatic_scopes_reject_zero_generic_counter_conflicts_and_transitive_links() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"seed","request_id":1,"source_session":1,"source_instance":1,"source_revision":7,"surface":"00000000000000010000000000000002","presentation_revision":11,"a":8,"b":9},{"event":"frame.published","request_id":1,"trace_id":55},{"event":"application.transitive","trace_id":55},{"event":"application.other_source","request_id":1,"source_session":2,"source_instance":1},{"event":"application.other_instance","request_id":1,"source_session":1,"source_instance":2},{"event":"application.other_frame","request_id":1,"source_revision":8},{"event":"application.other_publication","surface":"00000000000000010000000000000002","presentation_revision":12},{"event":"application.numeric_collision","a":8,"b":9,"value":7,"generation":1},{"event":"application.empty","request_id":0,"span_id":false,"trace_id":"000","operation_id":" "},{"event":"application.empty_strings","request_id":" 0 ","trace_id":"0000-0000"},{"event":"application.wrong_type","request_id":"1"},{"event":"frame.pixel","request_id":1}])json"));
 EQ(events(f.exported({"input.jsonl", "-q", "event=seed", "--auto-correlate"}).rows), (std::set<std::string>{"seed", "frame.published"}));
 EQ(f.query({"input.jsonl", "--auto-correlate"}).status, 2);
}
void test_automatic_ambiguous_projected_source_revisions_do_not_join() {
 FileFixture f;
 auto args = automatic_fixture(f);
 auto item = j(R"json({"event":"explore.frame.published","owner":"explore","source_session":1,"source_instance":2,"source_revision":7,"steady_ns":101})json");
 f.write("build/validation/auto-native.log", J::array({item}));
 auto result = f.exported(plus(args, {"--auto-correlate"}));
 FALSE(events(result.rows).contains("explore.frame.published"));
 CHECK(events(result.rows).contains("integration.explore_state"));
 item["event"] = "native.seed";
 f.write("build/validation/auto-native.log", J::array({item}));
 args.back() += " OR event=native.seed";
 FALSE(events(f.exported(plus(args, {"--auto-correlate"})).rows).contains("explore.frame.published"));
}
void test_automatic_control_proximity_is_labeled_and_bounded() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"application.snapshot","control":"explore.open","steady_ns":100000000},{"event":"seed","control":"explore.open","steady_ns":200000000},{"event":"application.too_late","control":"explore.open","steady_ns":1000000000},{"event":"application.other_control","control":"annotation.open","steady_ns":200000001},{"event":"application.generic_control","control":"work","steady_ns":200000001}])json"));
 Strings args = {"input.jsonl", "-q", "event=seed", "--auto-correlate"};
 auto result = f.exported(args);
 EQ(events(result.rows), (std::set<std::string>{"seed", "application.snapshot"}));
 auto automatic = matching_rows(result, "auto");
 HAS(only(automatic)["_log"]["auto_reason"], "time proximity");
 EQ(f.exported(plus(args, {"--near-ms", "50"})).rows.size(), 1U);
}
void test_automatic_bounds_diversity_global_capacity_and_existing_match_limit() {
 FileFixture f;
 J rows = J::array();
 for (int i = 0; i < 80; ++i) {
  rows.push_back({{"event", "seed"}, {"request_id", i + 1}, {"steady_ns", i * 100}});
  int offset = 0;
  for (auto event : {"frame.published", "application.snapshot", "application.ready", "application.completed", "frame.published"})
   rows.push_back({{"event", event}, {"request_id", i + 1}, {"steady_ns", i * 100 + ++offset}});
 }
 f.write("input.jsonl", rows);
 Strings args = {"input.jsonl", "-q", "event=seed", "--auto-correlate", "--limit", "150"};
 auto result = f.exported(args);
 EQ(result.status, 0);
 EQ(f.exported(args).rows, result.rows);
 auto automatic = matching_rows(result, "auto");
 EQ(automatic.size(), logs::MAX_AUTO_RELATED);
 EQ(matching_rows(result, "query").size(), 80U);
 std::map<std::string, int> by_anchor, by_identity;
 for (auto& r : automatic) {
  ++by_anchor[logs::encoded(r["_log"]["auto_anchor"])];
  ++by_identity[r["_log"]["auto_reason"].get<std::string>()];
 }
 CHECK(all(by_anchor, [](auto& item) { return item.second <= logs::MAX_AUTO_PER_ANCHOR; }));
 CHECK(all(by_identity, [](auto& item) { return item.second <= logs::MAX_AUTO_PER_IDENTITY; }));
 args.back() = "2";
 auto limited = f.exported(args);
 EQ(limited.rows.size(), 2U);
 EQ(matching_rows(limited, "query").size(), 2U);
 rows.clear();
 for (int i = 0; i < 20; ++i) {
  rows.push_back({{"event", "seed"}, {"phase_id", i + 1}, {"steady_ns", i * 100}});
  rows.push_back({{"event", "application.snapshot"}, {"phase_id", i + 1}, {"steady_ns", i * 100 + 1}});
 }
 f.write("weak.jsonl", rows);
 EQ(matching_rows(f.exported({"weak.jsonl", "-q", "event=seed", "--auto-correlate", "--limit", "100"}), "auto").size(), logs::MAX_AUTO_WEAK);
}
void test_automatic_respects_where_runs_and_explicit_correlation() {
 FileFixture f;
 auto args = automatic_fixture(f);
 f.write("build/validation/auto.jsonl.history/57-1000.jsonl", j(R"json([{"event":"explore.frame.published","owner":"explore","source_revision":7}])json"));
 auto result = f.exported(plus(args, {"--auto-correlate", "--where", "NOT event=presentation.frame.edge"}));
 EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/auto@current"});
 FALSE(events(result.rows).contains("presentation.frame.edge"));
 f.write("explicit.jsonl", j(R"json([{"event":"seed","request_id":1},{"event":"arbitrary","request_id":1}])json"));
 Strings explicit_args = {"explicit.jsonl", "-q", "event=seed", "--correlate", "request_id", "--format", "timeline"};
 EQ(f.query(explicit_args), f.query(plus(explicit_args, {"--no-auto-correlate"})));
 HAS(f.query(explicit_args).out, "[correlated]");
}
void test_automatic_payloads_and_scan_count_stay_bounded() {
 FileFixture f;
 f.write("input.jsonl", J::array({{{"event", "seed"}, {"request_id", 1}}, {{"event", "application.snapshot"}, {"request_id", 1}, {"detail", std::string(20000, 'x')}}}));
 Observe scans;
 auto result = f.exported({"input.jsonl", "-q", "event=seed", "--auto-correlate"});
 EQ(result.status, 0);
 EQ(scans.count("file.records"), 2U);
 auto automatic = matching_rows(result, "auto");
 auto& r = only(automatic);
 CHECK(r["_log"]["auto_payload_truncated"]);
 FALSE(r["_log"].contains("triage_payload_truncated"));
 CHECK(logs::chars(r["text"].get<std::string>()) <= logs::MAX_TRIAGE_TEXT + 1);
 CHECK(logs::chars(r["data"]["detail"].get<std::string>()) <= logs::MAX_TRIAGE_TEXT + 1);
}
void test_discovery_deduplicates_paths_and_excludes_histories_by_default() {
 FileFixture f;
 f.write("capture/trace.jsonl", j(R"json([{"event":"one"}])json"));
 f.write("capture/native.log", "plain\n");
 f.write("capture/trace.jsonl.history/previous.jsonl", j(R"json([{"event":"old"}])json"));
 f.write("capture/not-a-log.bin", "binary");
 EQ(project(discover({"capture", "capture/trace.jsonl"}, f.root), [](auto& s) { return s.source; }), (Strings{"capture/native.log", "capture/trace.jsonl"}));
 EQ(discover({"capture"}, f.root, true).size(), 3U);
 EQ(project(discover({"/host/repo/capture/*.log"}, f.root, false, "/host/repo"), [](auto& s) { return s.source; }), Strings{"capture/native.log"});
}
void test_discovery_captures_each_repeated_path_only_once() {
 FileFixture f;
 f.write("capture/trace.jsonl", j(R"json([{"event":"one"}])json"));
 f.write("capture/native.log", "plain\n");
 Observe captured;
 auto files = discover({"capture", "capture", "capture/*", "capture/trace.jsonl"}, f.root);
 EQ(files.size(), 2U);
 EQ(captured.count("file.capture"), 2U);
}
void test_directory_discovery_streams_entries_and_releases_handles() {
 FileFixture f;
 for (int i = 0; i < 3; ++i) f.write("capture/" + std::to_string(i) + ".log", "");
 struct Cancel {};
 Observe observed;
 try {
  logs::directory_logs(f.root / "capture", [&](const fs::path& p) {
   EQ(p.extension(), fs::path(".log"));
   EQ(observed.count("directory.entry"), 1U);
   throw Cancel{};
  });
 } catch (const Cancel&) {}
 EQ(observed.count("directory.close"), 1U);
}
void test_empty_missing_and_outside_inputs_fail_clearly() {
 FileFixture f;
 fs::create_directory(f.root / "empty");
 for (auto path : {"missing", "empty", "/etc/passwd"}) {
  auto result = f.query({path});
  EQ(result.status, 2);
  CHECK(result.err.starts_with("mmltk logs:"));
 }
}
void test_file_boundaries_blank_lines_and_oversized_records() {
 FileFixture f;
 auto path = f.write("input.log", "\n" + std::string(70, 'a') + "\n\nnext\n");
 auto source = logs::LogFile::capture(path, f.root);
 Rows rows;
 {
  Limit limit(mmltk::log_query::detail::limits.line_bytes, 32);
  rows = records(source);
 }
 EQ(project(rows, [](auto& r) { return r.line; }), (std::vector<std::size_t>{2, 4}));
 EQ(rows[0].raw.size(), 32U);
 CHECK(rows[0].parse_error);
 EQ(rows[1].raw, "next");
 FALSE(rows[1].parse_error);
 auto invalid = f.write("invalid.log", "invalid \xff bytes\n");
 EQ(records(invalid, f.root)[0].parse_error, "invalid UTF-8 bytes replaced");
}
void test_captured_byte_size_and_correlation_stability() {
 FileFixture f;
 auto path = f.write("input.log", "first\n");
 auto source = logs::LogFile::capture(path, f.root);
 {
  std::ofstream out(path, std::ios::app);
  out << "second\n";
 }
 EQ(project(records(source), [](auto& r) { return r.raw; }), Strings{"first"});
 raises([&] { records(source, true); }, "changed during correlation");
 support::write_text(path, "");
 raises([&] { records(source); }, "truncated");
}
void test_recent_input_preserves_physical_lines_and_limits_evidence() {
 FileFixture f;
 for (std::string ending : {"", "\n"}) {
  auto path =
   f.write("recent.jsonl", "malformed {\n" + std::string(1024 * 1024 + 7, 'x') + "\n{\"event\":\"earlier\",\"id\":7}\n\n{\"event\":\"ready\",\"id\":7}\n{\"event\":\"done\",\"id\":7}" + ending);
  auto result = f.exported({path.string(), "--last-lines", "3", "--strict", "-q", "@event=done", "--correlate", "id"});
  EQ(result.status, 0);
  EQ(column(result.rows, "_log", "line"), (std::vector<J>{5, 6}));
  includes(result.err, {"Partial input", "starting at line 4", "1 matched, 1 correlated"});
  auto source = logs::LogFile::capture(path, f.root).recent(1);
  {
   std::ofstream out(path, std::ios::app);
   out << "\nappended\n";
  }
  EQ(project(records(source), [](auto& r) { return r.line; }), std::vector<std::size_t>{6});
  raises([&] { records(source, true); }, "changed during correlation");
 }
 for (auto content : {"", "\n", "one", "one\n", "one\ntwo"}) {
  auto source = logs::LogFile::capture(f.write("short.log", content), f.root);
  auto values = [](auto& r) { return std::pair{r.line, r.raw}; };
  EQ(project(records(source.recent(10)), values), project(records(source), values));
 }
}
void test_retention_is_bounded_deterministic_and_counts_all_matches() {
 FileFixture f;
 f.write("b.jsonl", j(R"json([{"event":"tick","steady_ns":5},{"event":"tick","steady_ns":1},{"event":"tick","steady_ns":3}])json"));
 f.write("a.jsonl", j(R"json([{"event":"tick","steady_ns":4},{"event":"tick","steady_ns":2},{"event":"tick","steady_ns":3}])json"));
 auto result = f.exported({"*.jsonl", "--limit", "3"});
 EQ(result.status, 0);
 EQ(column(result.rows, "data", "steady_ns"), (std::vector<J>{1, 2, 3}));
 EQ(result.rows[2]["_log"]["file"], J("a.jsonl"));
 includes(result.err, {"6 matched", "showing 3/6"});
 EQ(column(f.exported({"*.jsonl", "--tail", "--limit", "2"}).rows, "data", "steady_ns"), (std::vector<J>{4, 5}));
 EQ(column(f.exported({"*.jsonl", "--order", "file", "--limit", "2"}).rows, "data", "steady_ns"), (std::vector<J>{4, 2}));
}
void test_clock_order_never_implies_cross_domain_alignment() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"untimed"},{"timestamp_ns":1},{"steady_ns":999},{"timestamp":"1970-01-01 00:00:01"},{"elapsed_ms":0.5}])json"));
 auto result = f.exported({"input.jsonl"});
 EQ(column(result.rows, "_log", "clock"), (std::vector<J>{"steady", "wall", "wall-local", "elapsed:input.jsonl", "none"}));
 HAS(result.err, "do not establish cross-domain order");
}
void test_correlation_is_direct_and_never_joins_empty_identities() {
 FileFixture f;
 f.write("native.jsonl", j(R"json([{"event":"seed","trace_id":1,"span_id":10},{"event":"zero-seed","trace_id":0}])json"));
 f.write("firefox.log",
  j(
   R"json([{"event":"direct","trace_id":1,"span_id":20},{"event":"transitive-only","trace_id":2,"span_id":20},{"event":"zero","trace_id":0},{"event":"missing"},{"event":"different-type","trace_id":"1"}])json"));
 auto result = f.exported({".", "-q", "event:seed", "--correlate", "trace_id", "--correlate", "span_id"});
 EQ(events(result.rows), (std::set<std::string>{"seed", "zero-seed", "direct"}));
 EQ(matching_rows(result, "correlated").size(), 1U);
}
void test_composite_identity_keeps_sessions_separate() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"seed","source_session":7,"source_instance":1},{"event":"same","source_session":7,"source_instance":1},{"event":"other-session","source_session":8,"source_instance":1},{"event":"other-instance","source_session":7,"source_instance":2}])json"));
 EQ(events(f.exported({"input.jsonl", "-q", "event=seed", "--correlate", "source_session+source_instance"}).rows), (std::set<std::string>{"seed", "same"}));
}
void test_memory_provenance_query_exposes_export_mapping_and_import_without_fd_identity_claim() {
 FileFixture f;
 auto native = j(R"json({"native_process_id":101,"workspace_descriptor":9,"workspace_plane":4096})json"), browser = j(R"json({"browser_process_id":202,"vk_device":"0x10","vk_image":"0x20"})json"),
      metadata = j(R"json({"fd_stat_status":0,"fd_dev":1,"fd_ino":2,"fd_identity_scope":"metadata_only_not_gpu_allocation_identity"})json");
 auto exported = updated(updated(native, metadata), {{"event", "cuda.workspace.memory_export"}});
 f.write("build/validation/memory-native.log",
  J::array(
   {updated(exported, {{"cuda_export_allocation", 55}, {"cuda_mapped_allocation", 56}, {"cuda_mapping_matches_export", false}, {"memory_size", 2097152}, {"row_pitch", 3584}}),
    updated(native, {{"event", "gpu.workspace.memory_export"}, {"workspace_allocation", 34}}), updated(exported, {{"native_process_id", 303}}), updated(exported, {{"workspace_plane", 8192}})}));
 f.write("build/validation/memory-firefox.log",
  J::array(
   {updated(browser, {{"event", "firefox.workspace.image_created"}, {"image_tiling", "LINEAR"}, {"image_usage", 5}, {"requirements_alignment", 256}}),
    updated(updated(browser, metadata), {{"event", "firefox.workspace.memory_import"}, {"workspace_allocation", 34}, {"workspace_descriptor", 77}, {"row_pitch", 3584}}),
    updated(browser, {{"event", "firefox.workspace.memory_bound"}, {"workspace_allocation", 34}, {"vk_memory", "0x30"}, {"image_binding_offset", 0}, {"fd_consumed", true}}),
    updated(browser, {{"event", "firefox.workspace.image_created"}, {"browser_process_id", 303}})}));
 Strings args = {
  "--family", "memory", "-q", "workspace_allocation=34", "--correlate", "native_process_id+workspace_descriptor+workspace_plane", "--correlate", "browser_process_id+vk_device+vk_image", "--limit", "8"
 };
 auto result = f.exported(args);
 EQ(result.status, 0);
 EQ(result.rows.size(), 5U);
 EQ(matching_rows(result, "correlated").size(), 2U);
 EQ(events(result.rows),
  (std::set<std::string>{"cuda.workspace.memory_export", "gpu.workspace.memory_export", "firefox.workspace.image_created", "firefox.workspace.memory_import", "firefox.workspace.memory_bound"}));
 auto timeline = f.query(plus(args, {"--format", "timeline"}));
 EQ(timeline.status, 0);
 includes(timeline.out, {"cuda_export_allocation=55", "cuda_mapped_allocation=56", "cuda_mapping_matches_export=false", "fd_ino=2", "image_tiling=LINEAR", "image_binding_offset=0",
                         "metadata_only_not_gpu_allocation_identity"});
}
using Mutation = std::function<void(J&, J&)>;
Strings descriptor_lineage_fixture(FileFixture& f, const Mutation& mutate = {}) {
 auto metadata = j(R"json({"fd_getfd_result":1,"fd_getfd_errno":0,"fd_stat_status":0,"fd_dev":1,"fd_ino":2,"fd_rdev":3,"fd_mode":8630,"fd_size":0})json");
 auto exported = j(R"json({"native_process_id":101,"workspace_descriptor":9,"workspace_plane":4096})json");
 auto transport = j(
  R"json({"source":"00000000000000010000000000000002","surface":"00000000000000030000000000000004","workspace_allocation":34,"native_process_id":101,"browser_process_id":202,"peer_credentials_known":true,"descriptor_count":4,"memory_descriptor_index":0,"record_bytes":256})json");
 auto imported =
  j(R"json({"source":"00000000000000010000000000000002","surface":"00000000000000030000000000000004","workspace_allocation":34,"browser_process_id":202,"vk_device":"0x10","vk_image":"0x20"})json");
 J native = J::array(
  {updated(updated(exported, metadata), {{"event", "cuda.workspace.memory_export"}, {"cuda_mapping_matches_export", true}}),
   updated(exported, {{"event", "gpu.workspace.memory_export"}, {"workspace_allocation", 34}}),
   updated(updated(transport, metadata), {{"event", "presentation.workspace.descriptor_duplicated"}, {"workspace_descriptor", 19}, {"export_descriptor", 9}, {"fd_ofd_status", "same"},
                                          {"fd_ofd_query", "F_DUPFD_QUERY"}, {"fd_ofd_result", 1}, {"fd_ofd_errno", 0}}),
   updated(updated(transport, metadata), {{"event", "presentation.workspace.descriptor_sent"}, {"workspace_descriptor", 19}, {"send_bytes", 256}, {"send_errno", 0}})});
 J browser = J::array(
  {updated(updated(transport, metadata), {{"event", "firefox.workspace.descriptor_received"}, {"workspace_descriptor", 77}}),
   updated(updated(transport, metadata), {{"event", "firefox.workspace.descriptor_claimed"}, {"workspace_descriptor", 77}}),
   updated(updated(imported, metadata), {{"event", "firefox.workspace.memory_import"}, {"workspace_descriptor", 77}}),
   updated(imported, {{"event", "firefox.workspace.memory_allocation"}, {"workspace_descriptor", 77}, {"vk_memory", "0x30"}, {"vk_allocate_status", 0}, {"fd_consumed", true},
                      {"fd_observation", "allocate_argument_not_post_consumption_query"}}),
   updated(imported, {{"event", "firefox.workspace.memory_bound"}, {"vk_memory", "0x30"}, {"fd_consumed", true}})});
 if (mutate) mutate(native, browser);
 f.write("build/validation/lineage-native.log", native);
 f.write("build/validation/lineage-firefox.log", browser);
 return {"--family", "lineage", "-q", "workspace_allocation=34", "--descriptor-lineage", "--limit", "32"};
}
void test_descriptor_lineage_joins_exact_source_and_process_scoped_exports() {
 FileFixture f;
 auto args = descriptor_lineage_fixture(f, [](J& native, J& browser) {
  native.push_back(updated(native[0], {{"native_process_id", 303}}));
  native.push_back(updated(native[0], {{"workspace_plane", 8192}}));
  browser.push_back(updated(browser.back(), {{"workspace_allocation", 99}, {"vk_image", "0x99"}}));
 });
 auto result = f.exported(args);
 EQ(result.status, 0);
 EQ(result.rows.size(), 9U);
 includes(result.err, {"native_pid=101 browser_pid=202", "duplicated(fd=19)[export_fd=9,ofd=same]", "received(fd=77)", "allocation(fd=77)[status=0,consumed=true]",
                       "conflicts: none observed in retained evidence", "not independent OFD/payload comparisons"});
 LACKS(result.err, "(missing)");
 auto timeline = f.query(plus(args, {"--format", "timeline"}));
 EQ(timeline.status, 0);
 includes(timeline.out, {"export_descriptor=9", "fd_ofd_query=F_DUPFD_QUERY", "fd_ofd_result=1", "vk_allocate_status=0"});
}
void test_descriptor_lineage_distinguishes_ofd_mismatch_unsupported_denied_and_unavailable() {
 FileFixture f;
 for (const auto& entry : j(R"json([["different",0,0],["unsupported",-1,22],["denied",-1,1],["invalid_descriptor",-1,9],["unavailable",-1,4]])json")) {
  auto args = descriptor_lineage_fixture(f, [&](J& native, J&) { native[2].update({{"fd_ofd_status", entry[0]}, {"fd_ofd_result", entry[1]}, {"fd_ofd_errno", entry[2]}}); });
  auto report = f.exported(args).err;
  HAS(report, "native duplicate OFD comparison: " + entry[0].get<std::string>());
  if (entry[0] == "different" || entry[0] == "invalid_descriptor")
   LACKS(report, "conflicts: none observed");
  else
   includes(report, {"conflicts: none observed in retained evidence", "unavailable: native duplicate OFD comparison:"});
 }
}
void test_descriptor_lineage_reports_fd_reuse_closed_fd_process_and_binding_conflicts() {
 FileFixture f;
 auto args = descriptor_lineage_fixture(f, [](J& native, J& browser) {
  native[2]["export_descriptor"] = 10;
  native[3]["workspace_descriptor"] = 20;
  browser[1]["workspace_descriptor"] = 78;
  browser[2].update({{"fd_getfd_result", -1}, {"fd_getfd_errno", 9}});
  browser[3].update({{"vk_allocate_status", -2}, {"fd_consumed", false}});
  browser[4].update({{"browser_process_id", 303}, {"vk_memory", "0x40"}, {"workspace_allocation", 35}});
 });
 auto report = f.exported(plus(args, {"-q", "source=\"00000000000000010000000000000002\" OR workspace_allocation=34"})).err;
 includes(report, {"workspace_descriptor differs", "browser_process_id differs", "workspace_allocation differs", "import fd_getfd_result=-1 errno=9", "vkAllocateMemory status=-2 fd_consumed=false",
                   "vk_memory differs", "cuda_export(missing)"});
}
void test_descriptor_lineage_checks_native_export_argument_without_metadata_identity_inference() {
 FileFixture f;
 auto report = f.exported(descriptor_lineage_fixture(f, [](J& native, J&) { native[2]["export_descriptor"] = 10; })).err;
 includes(report, {"workspace export FD differs from the native duplication argument", "Equal FD integers or fstat metadata never prove payload identity"});
}
void test_descriptor_lineage_rejects_ambiguous_reused_export_reference() {
 FileFixture f;
 auto report = f.exported(descriptor_lineage_fixture(f, [](J& native, J&) { native.push_back(updated(native[0], {{"cuda_mapping_matches_export", false}})); })).err;
 includes(report, {"ambiguous CUDA export association", "cuda_export(missing)"});
 LACKS(report, "CUDA mapping differs");
}
void test_descriptor_lineage_limits_and_where_remain_evidence_limits() {
 FileFixture f;
 auto args = descriptor_lineage_fixture(f);
 auto result = f.exported(plus(args, {"--where", "NOT @event=presentation.workspace.descriptor_sent"}));
 EQ(result.rows.size(), 8U);
 includes(result.err, {"sent(missing)", "conflicts: none observed in retained evidence"});
 result = f.exported(plus(args, {"--limit", "1"}));
 EQ(result.rows.size(), 1U);
 includes(result.err, {"retained physical rows only", "(missing)"});
 auto error = f.query({"--descriptor-lineage"});
 EQ(error.status, 2);
 HAS(error.err, "requires --query");
}
Strings vulkan_export_lineage_fixture(FileFixture& f, const Mutation& mutate = {}) {
 auto shared = j(R"json({"source":"00000000000000010000000000000002","workspace_allocation":34,"browser_process_id":202})json"),
      layout = j(R"json({"device_uuid":"00112233445566778899aabbccddeeff","device_incarnation":7,"memory_size":8192,"row_pitch":256,"image_offset":128,"dedicated":true})json");
 auto transport = updated(updated(shared, layout), {{"native_process_id", 101}, {"descriptor_count", 2}, {"memory_descriptor_index", 0}, {"record_bytes", 256}, {"peer_credentials_known", true},
                                                    {"initialization", "vulkan_external_ownership_settled"}}),
      physical = updated(shared, {{"vk_device", "0x10"}, {"vk_image", "0x20"}, {"vk_memory", "0x30"}, {"vk_status", 0}}),
      imported = updated(layout, {{"native_process_id", 101}, {"workspace_allocation", 34}, {"retained_memory_descriptor", 77}, {"cuda_status", 0}});
 auto send = updated(transport, {{"event", "firefox.workspace.descriptor_send"}, {"workspace_descriptor", 9}});
 J browser = J::array(
  {updated(physical, {{"event", "firefox.workspace.memory_allocation"}}), updated(physical, {{"event", "firefox.workspace.memory_bound"}}),
   updated(updated(physical, layout), {{"event", "firefox.workspace.memory_export"}, {"workspace_descriptor", 9}}), send,
   updated(send, {{"event", "firefox.workspace.descriptor_send_result"}, {"send_bytes", -1}, {"send_errno", EAGAIN}}), send,
   updated(send, {{"event", "firefox.workspace.descriptor_send_result"}, {"send_bytes", 256}, {"send_errno", 0}})});
 J native = J::array(
  {updated(transport, {{"event", "presentation.workspace.descriptor_received"}, {"workspace_descriptor", 77}}),
   updated(imported, {{"event", "cuda.workspace.memory_import_started"}, {"import_descriptor", 78}}),
   updated(imported, {{"event", "cuda.workspace.memory_import"}, {"import_descriptor", -1}, {"cuda_external_memory", 4096}, {"fd_consumed", true}}),
   updated(imported, {{"event", "cuda.workspace.memory_map"}, {"cuda_external_memory", 4096}, {"cuda_mapped_base", 8192}}),
   updated(imported, {{"event", "cuda.workspace.memory_retirement_started"}, {"cuda_external_memory", 4096}, {"cuda_mapped_base", 8192}}),
   updated(imported, {{"event", "cuda.workspace.memory_retirement"}, {"cuda_external_memory", 0}, {"cuda_mapped_base", 0}})});
 if (mutate) mutate(native, browser);
 f.write("build/validation/export-lineage-native.log", native);
 f.write("build/validation/export-lineage-firefox.log", browser);
 return {"--family", "export-lineage", "-q", "workspace_allocation=34", "--descriptor-lineage", "--limit", "64"};
}
void test_vulkan_export_lineage_preserves_descriptor_roles_and_retry_results() {
 FileFixture f;
 auto result = f.exported(vulkan_export_lineage_fixture(f));
 EQ(result.status, 0);
 EQ(result.rows.size(), 13U);
 includes(
  result.err, {"direction=Vulkan_export_to_CUDA_import", "send_attempt(count=2)", "native_received(fd=77)", "retained_fd=77,import_fd=78,status=0", "conflicts: none observed in retained evidence"});
 excludes(result.err, {"(missing)", "(ambiguous)"});
}
void test_vulkan_export_lineage_missing_success_and_conflicting_import_are_explicit() {
 FileFixture f;
 auto report = f.exported(vulkan_export_lineage_fixture(f, [](J& native, J& browser) {
  browser.erase(browser.end() - 1);
  native[3].update({{"row_pitch", 512}, {"cuda_status", 1}});
  native[1]["import_descriptor"] = 77;
 })).err;
 includes(report, {"send_result(no_unique_success)", "lacks an exact successful record-length result", "row_pitch differs", "cuda_map cuda_status=1", "not an independent duplicate"});
}
void test_vulkan_export_lineage_rejects_ambiguous_source_association() {
 FileFixture f;
 auto report = f.exported(vulkan_export_lineage_fixture(f, [](J& native, J&) { native.push_back(updated(native[0], {{"source", "00000000000000050000000000000006"}})); })).err;
 includes(report, {"ambiguous native process/allocation source association", "cuda_import(missing)"});
}
void test_vulkan_export_lineage_keeps_process_scoped_imports_and_null_success_visible() {
 FileFixture f;
 HAS(f.exported(vulkan_export_lineage_fixture(f, [](J& native, J&) { native.push_back(updated(native[3], {{"native_process_id", 303}, {"row_pitch", 4096}})); })).err,
  "conflicts: none observed in retained evidence");
 HAS(f.exported(vulkan_export_lineage_fixture(f, [](J& native, J&) { native[3]["cuda_mapped_base"] = 0; })).err, "cuda_map reports success with null cuda_mapped_base");
}
void test_vulkan_export_lineage_requires_retained_retirement_evidence() {
 FileFixture f;
 auto args = vulkan_export_lineage_fixture(f);
 auto report = f.exported(plus(args, {"--where", "NOT @event:memory_retirement"})).err;
 includes(report, {"retirement_started(missing)", "retirement(missing)", "conflicts: none observed in retained evidence", "missing/ambiguous stages are not proof of a failed handoff"});
 report = f.exported(vulkan_export_lineage_fixture(f, [](J& native, J&) { native.erase(native.begin() + 4, native.end()); })).err;
 includes(report, {"retirement_started(missing)", "retirement(missing)"});
}
void test_vulkan_export_lineage_retirement_requires_released_handles_and_same_backing() {
 FileFixture f;
 for (const auto& item :
  j(
   R"json([["cuda_mapped_base",8192,"retirement reports success with unreleased cuda_mapped_base"],["cuda_external_memory",4096,"retirement reports success with unreleased cuda_external_memory"],["cuda_status",1,"retirement cuda_status=1"],["retained_memory_descriptor",78,"retained_memory_descriptor differs"]])json")) {
  auto report = f.exported(vulkan_export_lineage_fixture(f, [&](J& native, J&) { native.back()[item[0].get<std::string>()] = item[1]; })).err;
  HAS(report, item[2].get<std::string>());
 }
}
void test_vulkan_export_lineage_compares_every_immutable_retry_observation() {
 FileFixture f;
 for (int index : {3, 4, 5})
  for (const auto& item : j(R"json([["workspace_descriptor",10],["row_pitch",512],["initialization","not_settled"],["record_bytes",128],["memory_descriptor_index",1],["dedicated",false]])json")) {
   auto report = f.exported(vulkan_export_lineage_fixture(f, [&](J&, J& browser) { browser[index][item[0].get<std::string>()] = item[1]; })).err;
   HAS(report, item[0].get<std::string>() + " differs");
   LACKS(report, "send_result(no_unique_success)");
  }
}
void test_vulkan_export_lineage_distinguishes_missing_source_association() {
 FileFixture f;
 for (bool separated : {false, true}) {
  auto report = f.exported(vulkan_export_lineage_fixture(f, [&](J& native, J& browser) {
   native[0].erase("native_process_id");
   for (auto& r : browser) r.erase("native_process_id");
   if (separated) {
    browser[0]["native_process_id"] = 101;
    browser[0].erase("workspace_allocation");
   }
  })).err;
  includes(report, {"missing native process/allocation source association", "cuda_import(missing)", "conflicts: none observed in retained evidence"});
  LACKS(report, "ambiguous native process/allocation source association");
 }
}
void test_vulkan_export_lineage_source_query_correlates_process_allocation() {
 FileFixture f;
 auto result = f.exported(plus(vulkan_export_lineage_fixture(f), {"-q", "source=\"00000000000000010000000000000002\""}));
 EQ(result.status, 0);
 CHECK(events(result.rows).contains("cuda.workspace.memory_map"));
 LACKS(result.err, "cuda_map(missing)");
}
void test_loaded_library_mapping_inventory_preserves_actual_paths_and_limits() {
 FileFixture f;
 const std::string path = "/usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.580.42.09";
 f.write("libraries.log",
  J::array(
   {{{"event", "firefox.workspace.loaded_library"}, {"browser_process_id", 202}, {"library_path", path}, {"library_provenance", "proc_self_maps_at_first_memory_import"}},
    j(
     R"json({"event":"firefox.workspace.library_inventory","browser_process_id":202,"inventory_status":"partial","inventory_errno":0,"inventory_bounded":true,"library_count":1,"maps_bytes":1048576,"maps_byte_limit":1048576})json"),
    j(R"json({"event":"cuda.workspace.library_inventory","native_process_id":101,"inventory_status":"unavailable","inventory_errno":13,"library_count":0})json")}));
 auto result = f.query({"libraries.log", "-q", "@event:loaded_library OR @event:library_inventory", "--format", "timeline"});
 EQ(result.status, 0);
 includes(result.out, {path, "browser_process_id=202", "native_process_id=101", "inventory_status=partial", "inventory_errno=13", "maps_byte_limit=1048576"});
}
void test_surface_correlates_native_and_firefox_and_where_scopes_all_rows() {
 FileFixture f;
 f.write("native.jsonl", j(R"json([{"event":"native.ready","surface_high":1,"surface_low":2}])json"));
 f.write("firefox.log", j(R"json([{"event":"firefox.ready","surface":"00000000000000010000000000000002"},{"event":"skip.me","surface":"00000000000000010000000000000002"}])json"));
 EQ(events(f.exported({".", "-q", "event=native.ready", "--correlate", "@surface", "--where", "NOT event=skip.me"}).rows), (std::set<std::string>{"native.ready", "firefox.ready"}));
}
void test_context_is_bounded_deduplicated_and_obeys_where() {
 FileFixture f;
 f.write("input.log", "before\nfailed first\nbetween\nfailed second\nafter\nexcluded\n");
 auto result = f.exported({"input.log", "-q", "failed", "--context", "2", "--where", "NOT excluded"});
 EQ(column(result.rows, "_log", "line"), (std::vector<J>{1, 2, 3, 4, 5}));
 EQ(column(result.rows, "_log", "match"), (std::vector<J>{"context", "query", "context", "query", "context"}));
}
void test_dense_context_considers_buffered_rows_only_once() {
 FileFixture f;
 constexpr int count = 512;
 f.write("input.log", repeat("match\n", count));
 Observe observed;
 auto result = f.exported({"input.log", "-q", "match", "--context", "100", "--limit", "1"});
 EQ(result.status, 0);
 EQ(result.rows.size(), 1U);
 includes(result.err, {"512 matched", "0 context"});
 CHECK(observed.count("context.visit") <= count);
}
void test_projection_preserves_provenance_and_uint64() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"one","trace_id":18446744073709551615}])json"));
 EQ(J(f.exported({"input.jsonl", "--fields", "event,trace_id,absent"}).rows),
  j(R"json([{"@file":"input.jsonl","@line":1,"@match":"query","event":"one","trace_id":18446744073709551615,"absent":null}])json"));
}
void test_error_filter_strict_mode_and_exit_statuses() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"pixel","error":0},{"event":"worker.failed","trace_id":5}])json"));
 auto result = f.exported({"input.jsonl", "--errors"});
 EQ(result.status, 0);
 EQ(column(result.rows, "data", "event"), std::vector<J>{"worker.failed"});
 EQ(f.query({"input.jsonl", "-q", "absent=5"}).status, 1);
 EQ(f.query({"input.jsonl", "-q", "event="}).status, 2);
 EQ(f.query({"input.jsonl", "--correlate", "trace_id"}).status, 2);
 f.write("broken.jsonl", "{\"event\":\"partial");
 auto strict = f.query({"broken.jsonl", "--strict"});
 EQ(strict.status, 2);
 HAS(strict.err, "1 malformed/truncated");
}
void test_summary_groups_are_deterministic_and_cardinality_is_bounded() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"b"},{"event":"a"},{"event":"b"},{"event":"c"}])json"));
 auto result = f.query({"input.jsonl", "--group-by", "event", "--top", "2"});
 EQ(result.status, 0);
 HAS(result.out, "      2  b\n        1  a\n");
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 2);
  result = f.query({"input.jsonl"});
 }
 EQ(result.status, 2);
 HAS(result.err, "group count exceeds 2");
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 2);
  result = f.query({"input.jsonl", "-q", "*", "--correlate", "event"});
 }
 EQ(result.status, 2);
 HAS(result.err, "correlation seeds exceed 2");
}
fs::path repository() { return support::repository_root({}, __FILE__); }
std::map<fs::path, std::size_t> path_counts(const Observe& observe, std::string_view event) {
 std::map<fs::path, std::size_t> result;
 for (auto& item : observe.events)
  if (item.event == event) ++result[fs::weakly_canonical(item.value.get<std::string>())];
 return result;
}
std::vector<fs::path> matching_files(const fs::path& root, std::string_view prefix, std::string_view suffix) {
 std::vector<fs::path> result;
 if (!fs::exists(root)) return result;
 for (const auto& entry : fs::directory_iterator(root)) {
  const auto name = entry.path().filename().string();
  if (name.starts_with(prefix) && name.ends_with(suffix)) result.push_back(entry.path());
 }
 return result;
}
using Environment = support::ScopedEnvironment;
void unset_wrapper_environment(support::RunOptions& options) {
 for (char** entry = ::environ; entry && *entry; ++entry) {
  std::string value(*entry);
  auto name = value.substr(0, value.find('='));
  if (name.starts_with("MMLTK_")) options.unset_env.push_back(name);
 }
}
struct WrapperFixture : FileFixture {
 fs::path wrapper;
 support::RunOptions options;
 explicit WrapperFixture(const std::string& script) {
  wrapper = root / "mmltk";
  fs::create_directories(root / "tools");
  fs::create_symlink(support::executable_path().parent_path() / "mmltk", wrapper);
  auto docker = write("bin/docker", script);
  fs::permissions(docker, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
  unset_wrapper_environment(options);
  options.env["MMLTK_DOCKER_CONTAINER"] = "host";
  options.env["PATH"] = docker.parent_path().string() + ":" + support::env("PATH");
  options.cwd = root;
  options.capture_stdout = options.capture_stderr = true;
  options.timeout = std::chrono::seconds(15);
 }
 support::ProcessResult invoke(const Strings& args) { return support::run(plus({wrapper.string()}, args), options); }
};
std::string start_ticks(pid_t pid) {
 auto text = support::read_text("/proc/" + std::to_string(pid) + "/stat");
 auto fields = support::split(text.substr(text.rfind(')') + 2), ' ', false);
 return fields.at(19);
}
// Native process fixture ownership; no interpreter implements the fake daemon.
class Child {
 pid_t pid_ = -1;
 bool group_ = false;
 std::optional<int> result_;

public:
 support::FileDescriptor input;
 Child(const Strings& args, const support::RunOptions& options, const fs::path& output, bool pipe_input = false, bool new_session = false) : group_(new_session) {
  auto pointers = support::detail::argument_pointers(args);
  support::CommandEnvironment environment(options.env, options.unset_env);
  support::FileDescriptor out(::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  CHECK(out.get() >= 0);
  auto pipe = pipe_input ? support::detail::pipe() : std::pair<support::FileDescriptor, support::FileDescriptor>{};
  support::FileDescriptor null(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  CHECK(null.get() >= 0);
  support::detail::SpawnActions actions;
  support::detail::SpawnAttributes attributes;
  CHECK(::posix_spawn_file_actions_addchdir_np(&actions.value, options.cwd.c_str()) == 0);
  for (int fd : {STDOUT_FILENO, STDERR_FILENO}) CHECK(::posix_spawn_file_actions_adddup2(&actions.value, out.get(), fd) == 0);
  CHECK(::posix_spawn_file_actions_adddup2(&actions.value, pipe_input ? pipe.first.get() : null.get(), STDIN_FILENO) == 0);
  if (new_session) CHECK(::posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETSID) == 0);
  auto error = ::posix_spawnp(&pid_, args[0].c_str(), &actions.value, &attributes.value, pointers.data(), environment.data());
  if (error) throw std::system_error(error, std::generic_category(), "spawn fixture");
  if (pipe_input) input = std::move(pipe.second);
 }
 Child(const Child&) = delete;
 Child& operator=(const Child&) = delete;
 ~Child() {
  if (pid_ > 0) {
   if (group_) ::kill(-pid_, SIGKILL);
   if (!result_) {
    ::kill(pid_, SIGKILL);
    int status;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
   }
  }
 }
 pid_t pid() const { return pid_; }
 std::optional<int> poll() {
  if (result_) return result_;
  int status = 0;
  const auto result = ::waitpid(pid_, &status, WNOHANG);
  if (result == pid_)
   result_ = WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
  else if (result < 0 && errno != EINTR)
   throw std::system_error(errno, std::generic_category(), "waitpid fixture");
  return result_;
 }
 int wait(int seconds = 5) {
  if (auto value = poll()) return *value;
  support::FileDescriptor fd(static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0)));
  CHECK(fd.get() >= 0);
  pollfd watch{fd.get(), POLLIN, 0};
  int ready;
  do { ready = ::poll(&watch, 1, seconds * 1000); } while (ready < 0 && errno == EINTR);
  CHECK(ready > 0);
  auto result = poll();
  CHECK(result);
  return *result;
 }
};
class FileNotifications {
 support::FileDescriptor notifications{::inotify_init1(IN_CLOEXEC)};

public:
 FileNotifications(const fs::path& path, std::uint32_t mask) {
  CHECK(notifications.get() >= 0);
  CHECK(::inotify_add_watch(notifications.get(), path.c_str(), mask) >= 0);
 }
 bool wait(int milliseconds) {
  pollfd watched{notifications.get(), POLLIN, 0};
  const int result = ::poll(&watched, 1, milliseconds);
  if (result < 0 && errno == EINTR) return false;
  CHECK(result > 0);
  std::array<char, 4096> events{};
  CHECK(::read(notifications.get(), events.data(), events.size()) > 0);
  return true;
 }
 void wait() {
  while (!wait(5000)) {}
 }
};
void wait_ready_file(const fs::path& path, int seconds = 5) {
 FileNotifications notifications(path.parent_path(), IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
 while (!fs::exists(path) || support::read_text(path).empty()) {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
  CHECK(remaining > 0);
  notifications.wait(static_cast<int>(remaining));
 }
}
void test_help_documents_grammar_and_native_wrapper_is_executable() {
 FileFixture f;
 auto result = f.query({"--help"});
 EQ(result.status, 0);
 includes(result.out, {"Grammar", "--correlate"});
 support::RunOptions options;
 options.capture_stdout = options.capture_stderr = true;
 EQ(support::read_text(repository() / "mmltk").substr(0, 4), std::string("\x7f"
                                                                         "ELF",
                                                              4));
 EQ(support::run({"bash", "-n", (repository() / "tools/bootstrap/bootstrap_mmltk.sh").string()}, options).exit_code, 0);
}
void test_wrapper_retains_both_streams_and_queryable_terminal_status() {
 WrapperFixture f(R"SH(#!/usr/bin/env bash
case "$1" in
    version|rm) exit 0 ;;
    image) printf 'fake-build-image\n' ;;
    run)
        for ((index = 0; index < 512; ++index)); do
            printf 'captured stdout %s\n' "$index"
            printf 'captured stderr %s\n' "$index" >&2
        done
        exit "${TEST_COMMAND_STATUS}"
        ;;
    *) exit 99 ;;
esac
)SH");
 for (const auto& [suite, expected] : std::vector<std::pair<std::string, int>>{{"log-query-tool", 0}, {"log-query-tool", 42}, {"unknown-suite", 1}}) {
  auto log_path = f.root / "captured logs" / (std::to_string(expected) + ".log");
  f.options.env["MMLTK_TEST_LOG_FILE"] = log_path.lexically_relative(f.root).string();
  f.options.env["TEST_COMMAND_STATUS"] = std::to_string(expected);
  auto result = f.invoke({"--test", suite});
  EQ(result.exit_code, expected);
  auto contents = support::read_text(log_path);
  CHECK(contents.ends_with("mmltk: test exited with status " + std::to_string(expected) + '\n'));
  HAS(result.err, "test exited with status " + std::to_string(expected));
  if (suite == "log-query-tool") {
   EQ(occurrences(result.out, "captured stdout "), 512U);
   EQ(occurrences(result.err, "captured stderr "), 512U);
   LACKS(result.out, "captured stderr ");
   LACKS(result.err, "captured stdout ");
   EQ(occurrences(contents, "captured stdout "), 512U);
   EQ(occurrences(contents, "captured stderr "), 512U);
  } else
   HAS(contents, "unknown test suite 'unknown-suite'");
  auto exported = f.exported({log_path.string(), "-q", "@event=process.terminal"});
  EQ(exported.status, 0);
  EQ(column(exported.rows, "data", "exit_code"), std::vector<J>{expected});
 }
 auto help_log = f.root / "help.log";
 f.options.env["MMLTK_TEST_LOG_FILE"] = help_log.string();
 EQ(f.invoke({"--test", "help"}).exit_code, 0);
 FALSE(fs::exists(help_log));
 FALSE(matching_files(f.root / ".cache/locks", "test-run-", ".owner"));
}
void test_fresh_all_stops_registered_tests_and_orphans_but_preserves_other_owners() {
 WrapperFixture f(R"SH(#!/usr/bin/env bash
printf '%s\n' "$*" >>"${TEST_DOCKER_CALLS}"
case "$1" in
    ps) cat "${TEST_CONTAINER_RECORDS}" ;;
    stop) exit 0 ;;
    rm) if [[ "$3" == orphan-container ]]; then : >"${TEST_CONTAINER_RECORDS}"; fi ;;
    version|run) exit 0 ;;
    image) printf 'fake-build-image\n' ;;
    *) exit 99 ;;
esac
)SH");
 auto calls = f.write("docker-calls.log", ""), containers = f.write("containers.txt", "orphan-container 99999999 1\n");
 f.options.env["TEST_DOCKER_CALLS"] = calls.string();
 f.options.env["TEST_CONTAINER_RECORDS"] = containers.string();
 const auto self = support::command::self_invocation(support::command::Command::test_log_query);
 std::vector<std::unique_ptr<Child>> children;
 for (int i = 0; i < 4; ++i) children.push_back(std::make_unique<Child>(plus(self, {"--fixture-pause", i == 3 ? "ignore-term" : "term"}), f.options, f.root / ("child-" + std::to_string(i)), true));
 auto& owner = *children[0];
 auto& stale = *children[1];
 auto& unrelated = *children[2];
 auto& stubborn = *children[3];
 // The pause helper writes its readiness to stdout before blocking. Wait for
 // that exact publication using inotify, rather than sleeping or polling.
 wait_ready_file(f.root / "child-3");
 EQ(support::trim(support::read_text(f.root / "child-3")), "ready");
 for (Child* child : {&owner, &stubborn}) f.write(".cache/locks/test-run-" + std::to_string(child->pid()) + ".owner", std::to_string(child->pid()) + ' ' + start_ticks(child->pid()) + '\n');
 f.write(".cache/locks/test-run-" + std::to_string(stale.pid()) + ".owner", std::to_string(stale.pid()) + " 0\n");
 f.write("other-checkout/.cache/locks/test-run-" + std::to_string(unrelated.pid()) + ".owner", std::to_string(unrelated.pid()) + ' ' + start_ticks(unrelated.pid()) + '\n');
 EQ(f.invoke({"--test", "log-query-tool"}).exit_code, 0);
 FALSE(owner.poll());
 FALSE(stubborn.poll());
 f.options.timeout = std::chrono::seconds(20);
 auto result = f.invoke({"--test", "all"});
 HAS(result.err, "stopping previous test run pid=" + std::to_string(owner.pid()));
 EQ(owner.wait(), -SIGTERM);
 HAS(result.err, "forcing previous test run to stop pid=" + std::to_string(stubborn.pid()));
 EQ(stubborn.wait(), -SIGKILL);
 FALSE(stale.poll());
 FALSE(unrelated.poll());
 auto commands = support::lines(support::read_text(calls));
 CHECK(any(commands, [](auto& c) { return contains(c, "stop --time 22 orphan-container"); }));
 CHECK(any(commands, [](auto& c) { return contains(c, "rm -f orphan-container"); }));
 auto queries = select(commands, [](auto& c) { return c.starts_with("ps "); });
 CHECK(queries.size() >= 2);
 for (int i = 0; i < 2; ++i) includes(queries[i], {"label=com.mmltk.repo=" + f.root.string(), "label=com.mmltk.mode=test"});
 FALSE(matching_files(f.root / ".cache/locks", "test-run-", ".owner"));
}
void send_ready(std::string_view stage, std::string_view owner) {
 support::FileDescriptor channel(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
 CHECK(channel.get() >= 0);
 const support::UnixAddress<Failure> address(support::env("TEST_RUNTIME_NOTIFY"), "fixture notify path too long");
 auto message = std::string(stage) + ' ' + std::string(owner);
 CHECK(::sendto(channel.get(), message.data(), message.size(), 0, address.get(), sizeof(address.value)) == static_cast<ssize_t>(message.size()));
}
std::string option_value(const Strings& args, std::string_view name) {
 const auto it = std::find(args.begin(), args.end(), name);
 return it == args.end() || std::next(it) == args.end() ? std::string{} : *std::next(it);
}
int runtime_docker(const Strings& args) {
 CHECK(!args.empty());
 const fs::path state = support::env("TEST_RUNTIME_STATE");
 const auto has = [&](std::string_view value) { return std::find(args.begin(), args.end(), value) != args.end(); };
 if (args[0] == "version" || args[0] == "image")
  std::cout << "fixture-image\n";
 else if ((args[0] == "ps" || args[0] == "container") && !has("inspect")) {
  Strings filters;
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
   if (args[i] == "--filter") filters.push_back(args[i + 1]);
  for (const auto& path : matching_files(state, "", ".json")) {
   auto labels = support::read_json(path);
   auto matches = [&](const std::string& filter) {
    if (filter.starts_with("label=")) {
     auto assignment = filter.substr(6);
     auto split = assignment.find('=');
     auto key = assignment.substr(0, split), expected = split == std::string::npos ? "" : assignment.substr(split + 1);
     return labels.contains(key) && (expected.empty() || labels[key] == expected);
    }
    return filter == "name=^/" + path.stem().string() + "$";
   };
   if (all(filters, matches)) {
    std::cout << path.stem().string();
    if (contains(option_value(args, "--format"), ".Label")) std::cout << ' ' << labels["com.mmltk.owner-pid"].get<std::string>() << ' ' << labels["com.mmltk.owner-start"].get<std::string>();
    std::cout << '\n';
   }
  }
 } else if (args[0] == "inspect" || (args.size() > 1 && args[0] == "container" && args[1] == "inspect")) {
  auto path = state / (args.back() + ".json");
  if (!fs::exists(path)) return 1;
  auto labels = support::read_json(path);
  auto format = option_value(args, "--format");
  for (auto it = labels.begin(); it != labels.end(); ++it)
   if (contains(format, it.key())) std::cout << it.value().get<std::string>() << '\n';
  if (contains(format, "State.Running")) std::cout << "true\n";
 } else if (args[0] == "run") {
  J labels = J::object();
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
   if (args[i] == "--label") {
    auto split = args[i + 1].find('=');
    labels[args[i + 1].substr(0, split)] = args[i + 1].substr(split + 1);
   }
  if (support::env("TEST_HOLD_CREATION") == "1") {
   send_ready("preparing", labels["com.mmltk.owner-pid"].get<std::string>());
   ::pause();
  }
  auto path = state / (option_value(args, "--name") + ".json"), temporary = path;
  temporary.replace_extension(".tmp");
  support::write_json(temporary, labels);
  fs::rename(temporary, path);
 } else if (args[0] == "rm" || args[0] == "stop")
  fs::remove(state / (args.back() + ".json"));
 else if (args[0] == "exec") {
  if (has("/opt/mmltk/bin/mmltk-browser-host") || has("/opt/mmltk/bin/mmltk"))
   for (const auto& value : args) {
    auto path = state / (value + ".json");
    if (!contains(value, "/") && fs::is_regular_file(path)) {
     send_ready("running", support::read_json(path)["com.mmltk.owner-pid"].get<std::string>());
     ::pause();
     break;
    }
   }
 } else
  throw Failure("unexpected Docker fixture command");
 return 0;
}
struct RuntimeFixture : WrapperFixture {
 fs::path state;
 support::FileDescriptor ready;
 std::vector<std::unique_ptr<Child>> children;
 using Launch = std::pair<Child*, fs::path>;
 RuntimeFixture()
     : WrapperFixture("#!/bin/sh\nexec " + support::command::shell_invocation(support::command::Command::test_log_query, {"--fixture-runtime-docker"}) + " \"$@\"\n"),
       state(root / "containers"),
       ready(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
  fs::create_directory(state);
  auto wayland = write("runtime/wayland-0", "fixture socket");
  CHECK(ready.get() >= 0);
  auto path = (root / "runtime-ready.sock").string();
  const support::UnixAddress<Failure> address(path, "fixture notify path too long");
  CHECK(::bind(ready.get(), address.get(), sizeof(address.value)) == 0);
  options.env["TEST_RUNTIME_STATE"] = state.string();
  options.env["TEST_RUNTIME_NOTIFY"] = path;
  options.env["XDG_RUNTIME_DIR"] = wayland.parent_path().string();
  options.env["WAYLAND_DISPLAY"] = wayland.filename().string();
  options.env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=" + write("runtime/bus", "fixture bus").string();
 }
 Launch launch(Strings args, bool preparing = false, bool deferred = false) {
  auto output = root / ("wrapper-" + std::to_string(children.size()) + ".log");
  auto command = plus({wrapper.string()}, args);
  if (deferred) command = plus({"bash", "-c", "read -r release; exec \"$@\"", "deferred-runtime", wrapper.string()}, args);
  auto settings = options;
  settings.env["TEST_HOLD_CREATION"] = preparing ? "1" : "0";
  auto process = std::make_unique<Child>(command, settings, output, deferred, true);
  auto child = process.get();
  children.push_back(std::move(process));
  return {child, output};
 }
 Launch replace_gui(const Launch& previous) {
  auto replacement = launch({"--gui"});
  wait_ready(replacement);
  NE(previous.first->wait(), 0);
  FALSE(replacement.first->poll());
  return replacement;
 }
 void wait_ready(const Launch& child, std::string_view stage = "running") {
  support::FileDescriptor pidfd(static_cast<int>(::syscall(SYS_pidfd_open, child.first->pid(), 0)));
  CHECK(pidfd.get() >= 0);
  std::array<pollfd, 2> watched{{{ready.get(), POLLIN, 0}, {pidfd.get(), POLLIN, 0}}};
  int result;
  do { result = ::poll(watched.data(), watched.size(), 45000); } while (result < 0 && errno == EINTR);
  if (result <= 0 || !(watched[0].revents & POLLIN)) throw Failure("runtime fixture did not become ready: " + support::read_text(child.second));
  std::array<char, 128> message{};
  auto count = ::recv(ready.get(), message.data(), message.size(), 0);
  CHECK(count >= 0);
  EQ(std::string(message.data(), count), std::string(stage) + ' ' + std::to_string(child.first->pid()));
 }
};
void test_gui_launch_replaces_runtime_sessions_and_preserves_other_owners() {
 RuntimeFixture f;
 auto start = start_ticks(::getpid());
 for (auto [name, repo, mode] : std::vector<std::tuple<std::string, fs::path, std::string>>{{"test-owner", f.root, "test"}, {"other-gui", f.root / "other", "gui"}})
  f.write("containers/" + name + ".json",
   J({{"com.mmltk.wrapper", "1"}, {"com.mmltk.repo", repo.string()}, {"com.mmltk.mode", mode}, {"com.mmltk.owner-pid", std::to_string(::getpid())}, {"com.mmltk.owner-start", start}}).dump());
 std::vector<RuntimeFixture::Launch> raw;
 for (int i = 0; i < 2; ++i) {
  raw.push_back(f.launch({"--help"}));
  f.wait_ready(raw.back());
 }
 fs::remove(f.root / (".cache/locks/runtime-run-" + std::to_string(raw[0].first->pid()) + ".owner"));
 auto first = f.launch({"--gui"});
 f.wait_ready(first);
 for (auto& child : raw) NE(child.first->wait(), 0);
 auto second = f.replace_gui(first);
 auto rejected = f.launch({"--help"});
 NE(rejected.first->wait(), 0);
 HAS(support::read_text(rejected.second), "raw mmltk invocation cannot start while the GUI is active");
 FALSE(second.first->poll());
 EQ(matching_files(f.root / ".cache/locks", "runtime-run-", ".owner").size(), 1U);
 EQ(matching_files(f.state, "", ".json").size(), 3U);
 CHECK(fs::exists(f.state / "test-owner.json"));
 CHECK(fs::exists(f.state / "other-gui.json"));
}
void test_gui_launch_replaces_an_owner_still_preparing_its_container() {
 RuntimeFixture f;
 auto first = f.launch({"--gui"}, true);
 f.wait_ready(first, "preparing");
 FALSE(matching_files(f.state, "", ".json"));
 auto second = f.replace_gui(first);
 EQ(matching_files(f.state, "", ".json").size(), 1U);
}
void test_delayed_older_gui_launch_preserves_the_newer_runtime() {
 RuntimeFixture f;
 auto earlier = f.launch({"--gui"}, false, true);
 auto raw = f.launch({"--help"});
 f.wait_ready(raw);
 auto newest = f.launch({"--gui"});
 f.wait_ready(newest);
 const std::string release = "continue\n";
 EQ(::write(earlier.first->input.get(), release.data(), release.size()), static_cast<ssize_t>(release.size()));
 NE(earlier.first->wait(), 0);
 HAS(support::read_text(earlier.second), "superseded by a newer runtime invocation");
 FALSE(newest.first->poll());
 EQ(matching_files(f.state, "", ".json").size(), 1U);
}
void test_native_compiler_launcher_preserves_arguments_cache_and_status() {
 FileFixture f;
 auto compiler = f.write("compiler with spaces", "#!/bin/sh\nprintf '%s\\0' \"$@\"\nexit \"${TEST_COMPILER_STATUS:-0}\"\n");
 auto cache = f.write("bin/ccache", "#!/bin/sh\nprintf 'cache\\n' >&2\nexec \"$@\"\n");
 for (const auto& path : {compiler, cache}) fs::permissions(path, fs::perms::owner_all);
 support::RunOptions options;
 options.capture_stdout = options.capture_stderr = true;
 options.env["PATH"] = cache.parent_path().string() + ':' + support::env("PATH");
 for (const auto& flag : {"-O2", "-fmodules", "-fmodules-ts", "-fmodule-mapper=path with spaces"}) {
  Strings args{compiler.string(), flag, "a b.cpp", "literal;$value"};
  auto result = support::run(support::command::self_invocation(support::command::Command::module_safe_compiler_launcher, args), options);
  EQ(result.exit_code, 0);
  EQ(support::split(result.out, '\0', false), Strings({flag, "a b.cpp", "literal;$value"}));
  EQ(result.err, std::string(flag) == "-O2" ? "cache\n" : "");
 }
 options.env["TEST_COMPILER_STATUS"] = "47";
 EQ(support::run(support::command::self_invocation(support::command::Command::module_safe_compiler_launcher, {compiler.string(), "-fmodules"}), options).exit_code, 47);
 EQ(support::run(support::command::self_invocation(support::command::Command::module_safe_compiler_launcher), options).exit_code, 2);
}
void populate_package(const fs::path& root) {
 for (const auto& row : support::runtime_package::inventory()) {
  fs::create_directories(root / row.path);
  for (auto entry : row.entries)
   if (!entry.starts_with('?')) support::write_text(root / row.path / entry, "payload\n");
 }
}
void test_native_package_inventory_completeness_and_stable_archive_identity() {
 namespace package = mmltk::tools::runtime_package;
 FileFixture f;
 const auto root = f.root / "package";
 populate_package(root);
 CHECK(package::complete(root));
 const auto initial = package::fingerprint(root);
 EQ(support::capture(support::command::self_invocation(support::command::Command::runtime_package, {"fingerprint", root.string()})).out, initial + '\n');
 for (const auto& path : {root / "bin", root / "missing", root / "bin/mmltk"}) {
  const auto result = support::capture(support::command::self_invocation(support::command::Command::runtime_package, {"path-fingerprint", path.string()}));
  EQ(result.exit_code, 0);
  EQ(result.out, package::path_fingerprint(path) + '\n');
 }
 const auto moved = f.root / "moved package";
 fs::rename(root, moved);
 EQ(package::fingerprint(moved), initial);
 support::write_text(moved / "bin/mmltk_unwanted_tests", "test executable");
 fs::permissions(moved / "bin/mmltk_unwanted_tests", fs::perms::owner_all);
 FALSE(package::complete(moved));
 fs::remove(moved / "bin/mmltk_unwanted_tests");
 support::write_text(moved / "bin/mmltk", "changed payload\n");
 NE(package::fingerprint(moved), initial);
 auto result = support::capture(support::command::self_invocation(support::command::Command::runtime_package, {"inventory"}));
 EQ(result.exit_code, 0);
 Strings fields;
 for (const auto& row : package::inventory()) {
  for (auto value : {row.path, row.destination, row.source_kind, row.source_path}) fields.emplace_back(value);
  for (auto value : row.entries) fields.emplace_back(value);
  fields.emplace_back();
 }
 std::string expected;
 for (const auto& field : fields) {
  expected += field;
  expected += '\0';
 }
 EQ(result.out, expected);
 fs::remove(moved / "models/ShiftLUT_fp32.onnx");
 FALSE(package::complete(moved));
}
// Only the external daemon is simulated. The wrapper still performs admission,
// hashing, child ownership, package publication and image-label verification.
int package_docker(const Strings& args) {
 CHECK(!args.empty());
 const fs::path root = support::env("TEST_PACKAGE_ROOT");
 const auto image_file = [&](const std::string& tag) { return root / "images" / (tag + ".json"); };
 const auto identity = "sha256:" + std::string(64, '1');
 if (args[0] == "image") {
  const auto format = option_value(args, "--format");
  if (format == "{{.Id}}")
   std::cout << identity << '\n';
  else if (format == "{{.Os}}/{{.Architecture}}")
   std::cout << "linux/amd64\n";
  else if (!format.empty() && fs::exists(image_file(args.at(2)))) {
   const auto labels = support::read_json(image_file(args.at(2)));
   for (auto it = labels.begin(); it != labels.end(); ++it)
    if (contains(format, "\"com.mmltk." + it.key() + "\"")) std::cout << it.value().get<std::string>() << '\n';
  }
 } else if (args[0] == "buildx" && args.at(1) == "inspect") {
  std::cout << "Driver: " << (args.at(2) == "default" ? "docker" : "docker-container") << '\n';
 } else if (args[0] == "buildx" && args.at(1) == "build") {
  J labels = J::object();
  const auto file = fs::path(option_value(args, "--file")).filename().string();
  if (file == "Dockerfile.runtime") labels = support::read_json(image_file("mmltk-runtime-base:latest"));
  for (std::size_t i = 0; i + 1 < args.size(); ++i)
   if (args[i] == "--build-arg") {
    const auto assignment = args[++i];
    const auto equals = assignment.find('=');
    auto key = assignment.substr(6, equals - 6);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return c == '_' ? '-' : static_cast<char>(std::tolower(c)); });
    if (key == "image-fingerprint") key = "input-fingerprint";
    if (key == "nvidia-donor-image") key = "nvidia-donor";
    labels[key] = assignment.substr(equals + 1);
   }
  if (file == "Dockerfile.ubuntu") labels["ubuntu-source-identity"] = labels["ubuntu-identity"];
  if (file == "Dockerfile.nvidia") labels["payload-mode"] = "extraction";
  if (file == "Dockerfile.toolchain") labels["payload-mode"] = "development";
  if (file == "Dockerfile.runtime-base") labels["payload-mode"] = "runtime";
  support::write_json(image_file(option_value(args, "--tag")), labels);
 } else if (args[0] == "run") {
  const auto name = option_value(args, "--name");
  support::write_json(root / "containers" / (name + ".json"), args);
  if (name.ends_with("-build-package")) {
   const auto stage = root / "build/release.next";
   fs::remove_all(stage);
   populate_package(stage);
   for (const auto& argument : args)
    if (argument.starts_with("MMLTK_PACKAGE_FINGERPRINT=")) support::write_text(stage / ".mmltk-package-fingerprint", argument.substr(26) + '\n');
  }
  if (name.ends_with("-upstream")) return 41;  // Stop before native numerical comparisons.
 } else if (args[0] != "version" && args[0] != "rm" && args[0] != "ps" && args[0] != "tag" && args[0] != "buildx")
  throw Failure("unexpected package Docker operation: " + args[0]);
 return 0;
}
struct PackageFixture : WrapperFixture {
 PackageFixture() : WrapperFixture("#!/bin/sh\nexec " + support::command::shell_invocation(support::command::Command::test_log_query, {"--fixture-package-docker"}) + " \"$@\"\n") {
  const auto native = support::executable_path().parent_path();
  fs::remove(wrapper);
  fs::copy_file(native / "mmltk", wrapper);
  fs::create_directory_symlink(native / "lib", root / "lib");
  options.env["TEST_PACKAGE_ROOT"] = root.string();
  options.env["PWD"] = root.string();
  options.env["MMLTK_USER"] = std::to_string(::getuid());
  options.timeout = std::chrono::seconds(45);
  fs::create_directory_symlink(repository() / "docker", root / "docker");
  for (const auto& entry : fs::directory_iterator(repository() / "tools"))
   if (entry.path().filename() != "bootstrap") fs::create_symlink(entry.path(), root / "tools" / entry.path().filename());
  fs::create_directories(root / "tools/bootstrap");
  write("tools/bootstrap/bootstrap_mmltk.sh", "#!/bin/sh\nset -eu\ncp -p \"$2/mmltk\" \"$2/.mmltk-next\"\nmv -f \"$2/.mmltk-next\" \"$2/mmltk\"\nreadlink \"/proc/$PPID/exe\" >\"$2/replaced-wrapper\"\n");
  write("build/tools/.inputs.sha256", "fixture tools\n");
  fs::create_symlink(support::executable_path().parent_path() / "mmltk", root / "build/tools/mmltk");
  for (const auto* path :
   {"src/entrypoints/desktop/firefox_runtime_build.cmake", "src/entrypoints/desktop/build_firefox_runtime.sh", "src/entrypoints/desktop/firefox_runtime_files.txt",
    "src/frontend/iced/wasm-opt-wrapper"})
   write(path, "fixture\n");
  write(".cache/cmake/release/build.ninja", "build mmltk_frontend_iced_bundle: phony\n");
  write(".cache/cmake/release/CMakeCache.txt", "BUILD_MMLTK_BROWSER_HOST:BOOL=ON\nBUILD_MMLTK_FIREFOX_RUNTIME:BOOL=ON\n");
  for (const auto& row : support::runtime_package::inventory()) {
   fs::path source;
   if (row.source_kind == "release")
    source = root / ".cache/cmake/release";
   else if (row.source_kind == "repository")
    source = root / row.source_path;
   else if (row.source_kind == "firefox")
    source = root / ".cache/firefox/obj-minimal-opt/dist/firefox";
   else if (row.source_kind == "browser")
    source = root / ".cache/cmake/release/frontend/iced/dist";
   else
    continue;
   fs::create_directories(source);
   for (auto entry : row.entries)
    if (!entry.starts_with('?')) support::write_text(source / entry, "payload\n");
  }
  write(".cache/cmake/release/generated/frontend/iced/browser_protocol.marker", "marker\n");
  for (const auto* directory : {"images", "containers", "descendants", "upstream/.git"}) fs::create_directories(root / directory);
  auto git = write("bin/git", R"SH(#!/bin/sh
case "$*" in
 *--absolute-git-dir*|*--git-common-dir*) printf '%s/upstream/.git\n' "$TEST_PACKAGE_ROOT";;
 *'rev-parse HEAD') printf 'e9a138f70cc14e3fddca029a0683055e34ab8f35\n';;
 *rev-parse*) printf 'fixture-tree\n';;
esac
)SH");
  fs::permissions(git, fs::perms::owner_all);
  for (const auto* role : {"tar", "sha256sum"}) {
   auto program =
    write(std::string("bin/") + role, "#!/bin/sh\nexec " + support::command::shell_invocation(support::command::Command::test_log_query, {"--fixture-package-child", role}) + " \"$@\"\n");
   fs::permissions(program, fs::perms::owner_all);
  }
 }
 void unchanged_release() {
  EQ(support::read_text(root / "build/release/sentinel"), "retained\n");
  FALSE(fs::exists(root / "build/release/.mmltk-package-fingerprint"));
  FALSE(fs::exists(root / "build/release.previous"));
  FALSE(fs::exists(root / "images/mmltk.json"));
 }
};
int package_child(const Strings& args) {
 CHECK(!args.empty());
 const auto parent = ::getppid();
 const auto command = support::split(support::read_text("/proc/" + std::to_string(parent) + "/cmdline"), '\0', false);
 const auto selected = std::find(command.begin(), command.end(), "runtime-package");
 const bool package = selected != command.end() && std::next(selected) != command.end() && std::find(selected, command.end(), support::env("TEST_PACKAGE_OPERATION")) != command.end();
 if (package && support::env("TEST_PACKAGE_HOLD") == "1") {
  const fs::path root = support::env("TEST_PACKAGE_ROOT");
  const auto record = root / "descendants" / std::to_string(::getpid());
  support::write_json(record, {{"pid", ::getpid()}, {"parent", parent}, {"role", args[0]}});
  fs::rename(record, record.string() + ".json");
  if (args[0] == "tar") support::write_text(support::env("TEST_PACKAGE_READY"), "ready\n");
  for (;;) ::pause();
 }
 if (package && support::env("TEST_PACKAGE_FAIL") == args[0]) {
  if (args[0] == "sha256sum") {
   std::array<char, 65536> buffer{};
   for (;;) {
    const auto count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    CHECK(count >= 0);
    if (!count) break;
   }
  }
  std::cerr << "fixture " << args[0] << " failed\n";
  return args[0] == "tar" ? 46 : 47;
 }
 auto invocation = args;
 invocation[0] = "/usr/bin/" + invocation[0];
 support::exec(invocation);
}
void test_package_callers_preserve_status_output_and_publication() {
 PackageFixture f;
 f.write("build/release/sentinel", "retained\n");
 for (const auto* operation : {"path-fingerprint", "fingerprint"}) {
  f.options.env["TEST_PACKAGE_OPERATION"] = operation;
  for (const auto* role : {"tar", "sha256sum"}) {
   f.options.env["TEST_PACKAGE_FAIL"] = role;
   const auto result = f.invoke({"--build"});
   EQ(result.exit_code, std::string_view(role) == "tar" ? 46 : 47);
   HAS(result.err, std::string("fixture ") + role + " failed");
   HAS(result.err, std::string("runtime-package: runtime package ") + role + " failed (exit ");
   f.unchanged_release();
  }
 }
 f.options.env.erase("TEST_PACKAGE_FAIL");
 const auto result = f.invoke({"--build"});
 if (result.exit_code) throw Failure(result.err);
 CHECK(support::read_text(f.root / "replaced-wrapper").ends_with(" (deleted)\n"));
 CHECK(support::runtime_package::complete(f.root / "build/release"));
 FALSE(fs::exists(f.root / "build/release/sentinel"));
 // The input stored in the image includes the command's exact digest newline.
 const auto files = support::capture({"sha256sum", (f.root / "docker/Dockerfile.runtime").string(), (f.root / "tools/toolchain/runtime_package.cpp").string()});
 EQ(files.exit_code, 0);
 support::RunOptions hashing;
 hashing.capture_stdout = true;
 hashing.input = "mmltk-runtime-image-v6\nsha256:" + std::string(64, '1') + '\n' + files.out + support::runtime_package::fingerprint(f.root / "build/release") + "\nfixture tools";
 const auto digest = support::run({"sha256sum"}, hashing);
 EQ(digest.exit_code, 0);
 EQ(support::read_json(f.root / "images/mmltk.json").at("release-fingerprint"), digest.out.substr(0, 64));
}
void test_package_callers_cancel_and_reap_archive_and_hash_descendants() {
 int previous = 0;
 CHECK(::prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0);
 CHECK(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
 struct Reaper {
  int previous;
  std::set<pid_t> descendants{};
  ~Reaper() {
   for (const auto pid : descendants) ::kill(pid, SIGKILL);
   for (const auto pid : descendants)
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
   ::prctl(PR_SET_CHILD_SUBREAPER, previous);
  }
 } reaper{previous};
 for (const auto* operation : {"path-fingerprint", "fingerprint"})
  for (const auto number : {SIGINT, SIGTERM}) {
   PackageFixture f;
   f.write("build/release/sentinel", "retained\n");
   const auto ready = f.write("package-ready", "");
   f.options.env["TEST_PACKAGE_OPERATION"] = operation;
   f.options.env["TEST_PACKAGE_HOLD"] = "1";
   f.options.env["TEST_PACKAGE_READY"] = ready.string();
   Child wrapper({f.wrapper.string(), "--build"}, f.options, f.root / "wrapper.log", false, true);
   wait_ready_file(ready, 45);
   // The archive cannot start until its HashStreams have spawned. Wait
   // for their publication as well, even if a hash child has not run yet.
   const auto archive_records = matching_files(f.root / "descendants", "", ".json");
   CHECK(!archive_records.empty());
   const auto helper = support::read_json(archive_records.front()).at("parent").get<pid_t>();
   const auto children = support::split(support::trim(support::read_text("/proc/" + std::to_string(helper) + "/task/" + std::to_string(helper) + "/children")), ' ', false);
   EQ(children.size(), std::string_view(operation) == "fingerprint" ? 3U : 2U);
   for (const auto& child : children) wait_ready_file(f.root / "descendants" / (child + ".json"));
   std::set<std::string> roles;
   std::set<pid_t> helpers;
   for (const auto& file : matching_files(f.root / "descendants", "", ".json")) {
    const auto process = support::read_json(file);
    const auto pid = process.at("pid").get<pid_t>();
    CHECK(::kill(pid, 0) == 0);
    reaper.descendants.insert(pid);
    helpers.insert(process.at("parent").get<pid_t>());
    roles.insert(process.at("role").get<std::string>());
   }
   EQ(roles, (std::set<std::string>{"tar", "sha256sum"}));
   EQ(helpers.size(), 1U);
   NE(*helpers.begin(), wrapper.pid());
   EQ(::kill(wrapper.pid(), number), 0);
   EQ(wrapper.wait(10), 128 + number);
   while (!reaper.descendants.empty()) {
    const auto pid = *reaper.descendants.begin();
    support::FileDescriptor event(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
    CHECK(event.get() >= 0);
    pollfd watch{event.get(), POLLIN, 0};
    int ready;
    do { ready = ::poll(&watch, 1, 2000); } while (ready < 0 && errno == EINTR);
    CHECK(ready > 0);
    int status = 0;
    EQ(::waitpid(pid, &status, 0), pid);
    reaper.descendants.erase(pid);
    CHECK(WIFSIGNALED(status));
    EQ(WTERMSIG(status), SIGKILL);
   }
   EQ(::kill(*helpers.begin(), 0), -1);
   EQ(errno, ESRCH);
   f.unchanged_release();
  }
}
void test_native_process_selection_artifact_bounds_and_actions() {
 FileFixture f;
 support::RunOptions options;
 options.cwd = f.root;
 options.capture_stdout = options.capture_stderr = true;
 options.timeout = std::chrono::seconds(10);
 const auto name = "mmltk-selector-" + std::to_string(::getpid());
 const auto target = f.root / name;
 fs::copy_file("/usr/bin/sleep", target);
 const auto artifact = f.write("artifact", std::string("a\0b\n", 4));
 const auto inspect = [&](Strings args) { return support::run(support::command::self_invocation(support::command::Command::diagnose_native_process, args), options); };
 const auto read = [&](const std::string& executable, Strings selector = {}) { return inspect(plus({"--read-file", executable, "--file", artifact.string()}, selector)); };
 auto absent = read(name);
 EQ(absent.exit_code, 2);
 HAS(absent.err, "matching container PIDs: []");
 Child first({target.string(), "120"}, options, f.root / "first.log");
 auto single = read(name);
 EQ(single.exit_code, 0);
 HAS(single.out, "container pid=" + std::to_string(first.pid()));
 HAS(single.out, "b'a\\x00b\\n'");
 Child second({target.string(), "120"}, options, f.root / "second.log");
 const auto ambiguous = read(name);
 EQ(ambiguous.exit_code, 2);
 for (auto pid : {first.pid(), second.pid()}) HAS(ambiguous.err, std::to_string(pid));
 const Strings selected{"--pid", std::to_string(first.pid())};
 EQ(read(name, selected).exit_code, 0);
 EQ(read(name + "-suffix", selected).exit_code, 2);
 EQ(read(name, {"--pid", "999999999999999999999999999999"}).exit_code, 2);
 support::write_text(artifact, std::string(4 * 1024 * 1024, 'x'));
 const auto limit = read(name, selected);
 EQ(limit.exit_code, 0);
 HAS(limit.out, "bytes=4194304\n");
 fs::resize_file(artifact, 4 * 1024 * 1024 + 1);
 const auto oversized = read(name, selected);
 EQ(oversized.exit_code, 2);
 HAS(oversized.err, "no larger than 4 MiB");
 fs::remove(artifact);
 CHECK(::mkfifo(artifact.c_str(), 0600) == 0);
 EQ(read(name, selected).exit_code, 2);
 fs::remove(artifact);
 support::write_text(artifact, "retained\n");
 const auto debugger = f.write("bin/gdb", "#!/bin/sh\nprintf '%s\\0' \"$@\" >\"${TEST_DEBUGGER_ARGS}\"\nexit \"${TEST_DEBUGGER_STATUS}\"\n");
 fs::permissions(debugger, fs::perms::owner_all);
 options.env["PATH"] = debugger.parent_path().string() + ':' + support::env("PATH");
 options.env["TEST_DEBUGGER_ARGS"] = (f.root / "debugger-args").string();
 for (const int status : {0, 43}) {
  options.env["TEST_DEBUGGER_STATUS"] = std::to_string(status);
  EQ(inspect(plus({"--backtrace", name}, selected)).exit_code, status);
 }
 EQ(support::split(support::read_text(f.root / "debugger-args"), '\0', false),
  Strings({"--batch", "--nx", "--quiet", "-p", std::to_string(first.pid()), "-ex", "set pagination off", "-ex", "set print thread-events off", "-ex", "thread apply all bt", "-ex", "detach"}));
 EQ(inspect(plus({"--terminate", name}, selected)).exit_code, 0);
 EQ(first.wait(), -SIGTERM);
 EQ(read(name).exit_code, 0);
 // A different process executing this same native binary remains selectable.
 const auto paused_log = f.write("same-binary.log", "");
 Child same(support::command::self_invocation(support::command::Command::test_log_query, {"--fixture-pause"}), options, paused_log);
 wait_ready_file(paused_log);
 const auto native_name = support::executable_path().filename().string();
 const auto native = read(native_name, {"--pid", std::to_string(same.pid())});
 EQ(native.exit_code, 0);
 HAS(native.out, "container pid=" + std::to_string(same.pid()));
 const auto self = support::run(support::command::self_invocation(support::command::Command::test_log_query, {"--fixture-inspector-self", artifact.string()}), options);
 EQ(self.exit_code, 2);
 HAS(self.err, "matching container PIDs: []");
 EQ(inspect({"--terminate", native_name, "--pid", std::to_string(same.pid())}).exit_code, 0);
 EQ(same.wait(), -SIGTERM);
}
void test_tools_tidy_and_oracle_mount_diagnostic_runtime_readonly() {
 PackageFixture f;
 const auto tidy = f.invoke({"--tidy-tools"});
 if (tidy.exit_code) throw Failure(tidy.err);
 f.write("upstream/src/rfdetr/models/lwdetr.py", "fixture\n");
 f.write(".cache/mmltk/weights/rf-detr-nano.pth", "fixture\n");
 f.write(".cache/mmltk/weights/rf-detr-seg-nano.pt", "fixture\n");
 const auto oracle = f.invoke({"--diagnose-rfdetr-upstream", "--upstream", (f.root / "upstream").string(), "--output", (f.root / "evidence").string()});
 EQ(oracle.exit_code, 41);
 std::set<std::string> checked;
 for (const auto& path : matching_files(f.root / "containers", "", ".json")) {
  const auto arguments = support::read_json(path).get<Strings>();
  const auto name = path.stem().string();
  const bool oracle = name.ends_with("-upstream");
  if (!oracle && !name.ends_with("-tidy")) continue;
  checked.insert(oracle ? "oracle" : "tidy");
  const auto mount = f.root.string() + "/build/tools:/opt/mmltk-tools:ro";
  const auto found = std::find(arguments.begin(), arguments.end(), mount);
  CHECK(found != arguments.end());
  CHECK(found != arguments.begin());
  EQ(*std::prev(found), "--volume");
  CHECK(fs::exists(f.root / "build/tools/mmltk"));
  CHECK(std::find(arguments.begin(), arguments.end(), "com.mmltk.wrapper=1") != arguments.end());
  if (oracle) {
   for (const auto* value : {"--read-only", "ALL", "no-new-privileges", "none", "/opt/rfdetr-oracle/mmltk-rfdetr-upstream-oracle", "7200s"})
    CHECK(std::find(arguments.begin(), arguments.end(), value) != arguments.end());
   CHECK(std::find(arguments.begin(), arguments.end(), (f.root / "upstream").string() + ":/upstream:ro") != arguments.end());
   CHECK(std::find(arguments.begin(), arguments.end(), (f.root / "evidence").string() + ":/evidence:rw") != arguments.end());
  } else {
   CHECK(std::find(arguments.begin(), arguments.end(), "/workspace/build/tools/mmltk") != arguments.end());
   CHECK(std::find(arguments.begin(), arguments.end(), "--tools-only") != arguments.end());
  }
 }
 EQ(checked, (std::set<std::string>{"oracle", "tidy"}));
}
void test_tools_tidy_grouped_selection_and_module_resume() {
 FileFixture f;
 auto calls = f.write("calls", "");
 for (const auto& file : {"a.cpp", "b.cpp", "c.cpp"}) f.write(std::string("tools/") + file, "export module fixture;\n");
 const auto build = f.root / ".cache/cmake/tools";
 J entries = J::array();
 for (const auto& file : {"a.cpp", "b.cpp", "c.cpp"})
  entries.push_back(
   {{"directory", build.string()}, {"file", (f.root / "tools" / file).string()}, {"output", std::string("CMakeFiles/fixture.dir/") + file + ".o"},
    {"arguments", Strings{"/opt/gcc-16.2/bin/g++", "-freflection", "-fmodules"}}});
 f.write(".cache/cmake/tools/compile_commands.json", entries.dump());
 const std::map<std::string, std::string> programs{
  {"git", "case \"$*\" in *--error-unmatch*|*--deleted*) exit 0;; esac\nprintf 'tools/a.cpp\\0tools/b.cpp\\0tools/c.cpp\\0'\n"}, {"nproc", "echo 2\n"}, {"clang-format", "exit 0\n"},
  {"clang-tidy", "exit 99\n"}, {"cmake", "printf '%s\\n' \"$*\" >>\"${TEST_TOOL_CALLS}\"\n"}
 };
 for (const auto& [name, script] : programs) {
  auto path = f.write("bin/" + name, "#!/bin/sh\n" + script);
  fs::permissions(path, fs::perms::owner_all);
 }
 support::RunOptions options;
 options.capture_stdout = options.capture_stderr = true;
 options.env = {
  {"PATH", (f.root / "bin").string() + ':' + support::env("PATH")}, {"MMLTK_REPO_ROOT", f.root.string()}, {"MMLTK_TIDY_BUILD_DIR", build.string()},
  {"MMLTK_TIDY_LOG_FILE", (f.root / "tidy.log").string()}, {"MMLTK_TIDY_TEE_STARTED", "1"}, {"LD_LIBRARY_PATH", support::env("LD_LIBRARY_PATH", "/fixture")}, {"TEST_TOOL_CALLS", calls.string()}
 };
 auto run_tidy = [&](const Strings& selection) {
  support::write_text(calls, "");
  auto arguments = plus({"--tools-only"}, selection);
  auto result = support::run(support::command::self_invocation(support::command::Command::run_static_analysis, arguments), options);
  if (result.exit_code) throw Failure(result.err);
  return support::read_text(calls);
 };
 auto grouped = run_tidy({"--file", "tools/a.cpp", "tools/c.cpp"});
 includes(grouped, {"-S " + (f.root / "tools").string(), "a.cpp.o", "c.cpp.o"});
 LACKS(grouped, "b.cpp.o");
 LACKS(grouped, "--preset");
 auto resumed = run_tidy({"--start-at", "tools/b.cpp"});
 includes(resumed, {"b.cpp.o", "c.cpp.o"});
 LACKS(resumed, "a.cpp.o");
}
void check_tools_handoff_consumed(const fs::path& root) {
 for (const auto* key : {"MMLTK_BOOTSTRAP_LOCK_FD", "MMLTK_BOOTSTRAP_CHECKOUT", "MMLTK_BOOTSTRAP_CACHE", "MMLTK_BOOTSTRAP_STAGE"}) CHECK(std::getenv(key) == nullptr);
 const auto lock = root / ".cache/locks/tools.lock";
 for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
  std::error_code error;
  const auto target = fs::read_symlink(entry.path(), error);
  if (!error) NE(target, lock);
 }
}
void tools_fixture_user(std::string_view identity) {
 const auto fields = support::split(identity, ':');
 EQ(fields.size(), 2U);
 const auto uid = static_cast<uid_t>(std::stoul(fields[0]));
 const auto gid = static_cast<gid_t>(std::stoul(fields[1]));
 if (::getuid() == 0 && (uid != ::getuid() || gid != ::getgid())) {
  CHECK(::setgroups(0, nullptr) == 0);
  CHECK(::setgid(gid) == 0);
  CHECK(::setuid(uid) == 0);
 }
 EQ(::getuid(), uid);
 EQ(::getgid(), gid);
}
support::FileDescriptor tools_lock(const fs::path& root) {
 support::FileDescriptor lock(::open((root / ".cache/locks/tools.lock").c_str(), O_RDWR | O_CLOEXEC));
 CHECK(lock.get() >= 0);
 return lock;
}
int tools_docker(const Strings& arguments) {
 CHECK(!arguments.empty());
 const fs::path root = support::env("TEST_BOOTSTRAP_ROOT");
 check_tools_handoff_consumed(root);
 const auto& operation = arguments[0];
 const auto active = root / "build/tools-writer";
 if (operation == "rm" || operation == "ps") {
  if (!fs::exists(active / "owner.json")) return 0;
  const auto owner = support::read_json(active / "owner.json");
  if (operation == "ps")
   std::cout << owner.at("name").get<std::string>() << '\n';
  else if (owner.at("name") == arguments.back()) {
   const auto ready = support::env("TEST_TOOLS_CLEANUP_READY");
   if (!ready.empty()) {
    support::write_text(ready, "cleanup\n");
    std::string release;
    CHECK(static_cast<bool>(std::getline(std::cin, release)));
    EQ(release, "continue");
   }
   fs::remove_all(active);
  }
  return 0;
 }
 if (operation == "version" || operation == "tag" || operation == "buildx") return 0;
 if (operation == "image") {
  if (std::find(arguments.begin(), arguments.end(), "{{.Id}}") != arguments.end()) std::cout << "sha256:" << std::string(63, '0') << "1\n";
  return 0;
 }
 EQ(operation, "run");
 std::map<std::string, std::string> environment;
 std::string identity, name;
 for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
  if (arguments[i] == "--env") {
   const auto& assignment = arguments[++i];
   const auto at = assignment.find('=');
   environment[assignment.substr(0, at)] = assignment.substr(at + 1);
  } else if (arguments[i] == "--user")
   identity = arguments[++i];
  else if (arguments[i] == "--name")
   name = arguments[++i];
 }
 if (std::find(arguments.begin(), arguments.end(), "log-query") != arguments.end()) return 0;
 const bool build = environment.contains("MMLTK_TOOLS_STAGE");
 const std::string kind = build ? "build" : "tidy";
 auto host_path = [&](const std::string& value) {
  CHECK(value.starts_with("/workspace/"));
  return root / value.substr(11);
 };
 const auto cache = host_path(environment.at(build ? "MMLTK_TOOLS_BUILD" : "MMLTK_TIDY_BUILD_DIR"));
 auto lock = tools_lock(root);
 EQ(::flock(lock.get(), LOCK_EX | LOCK_NB), -1);
 EQ(errno, EWOULDBLOCK);
 {
  std::ofstream calls(support::env("TEST_BOOTSTRAP_CALLS"), std::ios::app);
  calls << J({{"kind", kind}, {"user", identity}, {"cache", cache.string()}}).dump() << '\n';
  CHECK(calls.good());
 }
 tools_fixture_user(identity);
 CHECK(fs::create_directory(active));
 struct Writer {
  fs::path path;
  ~Writer() {
   std::error_code error;
   fs::remove_all(path, error);
  }
 } writer{active};
 support::write_json(active / "owner.json", {{"name", name}, {"pid", ::getpid()}});
 fs::create_directories(cache);
 if (build) support::write_text(cache / "compile_commands.json", "[");
 const auto ready = support::env("TEST_BOOTSTRAP_READY");
 if (!ready.empty()) {
  support::write_text(ready, std::to_string(::getpid()));
  for (;;) ::pause();
 }
 if (support::env("TEST_TOOLS_HOLD") == kind) {
  support::write_text(support::env("TEST_TOOLS_READY"), kind + "\n");
  std::string release;
  CHECK(static_cast<bool>(std::getline(std::cin, release)));
  EQ(release, "continue");
 }
 if ((build && support::env("TEST_BOOTSTRAP_FAILURE") == "1") || support::env("TEST_TOOLS_FAILURE") == kind) return 43;
 if (build) {
  support::write_json(cache / "compile_commands.json", J::array({{{"file", "/workspace/tools/input.cpp"}}}));
  const auto stage = host_path(environment.at("MMLTK_TOOLS_STAGE"));
  fs::create_directories(stage);
  fs::copy_file(support::env("TEST_BOOTSTRAP_BINARY"), stage / "mmltk");
  fs::create_directory_symlink(fs::path(support::env("TEST_BOOTSTRAP_BINARY")).parent_path() / "lib", stage / "lib");
  fs::create_symlink(support::executable_path(), stage / "mmltk-tests");
 } else
  support::write_text(cache / "tidy-object", "checked\n");
 return 0;
}
struct NativeToolsFixture : FileFixture {
 fs::path checkout = root / "checkout with spaces";
 fs::path calls;
 support::RunOptions options;
 NativeToolsFixture() {
  const auto& root = checkout;
  fs::create_directories(root / "tools/bootstrap");
  fs::create_directories(root / "docker");
  fs::create_directories(root / "third_party/json/include");
  fs::create_directory_symlink(support::executable_path().parent_path() / "lib", root / "lib");
  support::write_text(root / "mmltk", support::read_text(repository() / "mmltk"));
  support::write_text(root / "tools/bootstrap/bootstrap_mmltk.sh", support::read_text(repository() / "tools/bootstrap/bootstrap_mmltk.sh"));
  fs::permissions(root / "mmltk", fs::perms::owner_all);
  fs::permissions(root / "tools/bootstrap/bootstrap_mmltk.sh", fs::perms::owner_all);
  for (const auto* path : {"docker/Dockerfile.gcc", "docker/Dockerfile.tools", "tools/input.cpp"}) support::write_text(root / path, "fixture\n");
  support::write_text(root / "docker/nvidia-payload.json", "{\"donor\": \"fixture@sha256:0123456789abcdef\",\n\"cuda_source\": \"/usr/local/cuda-fixture\"}\n");
  fs::create_symlink(support::executable_path().parent_path() / "mmltk", root / "native");
  auto docker = write("bin/docker", "#!/bin/sh\nexec " + support::command::shell_invocation(support::command::Command::test_log_query, {"--fixture-tools-docker"}) + " \"$@\"\n");
  auto flock = write("bin/flock", "#!/bin/sh\nif [ -n \"${TEST_TOOLS_ADMISSION:-}\" ] && [ \"$1\" = -x ]; then printf 'waiting\\n' >\"${TEST_TOOLS_ADMISSION}\"; fi\nexec /usr/bin/flock \"$@\"\n");
  for (const auto& path : {docker, flock}) fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec);
  options.cwd = checkout;
  options.capture_stdout = options.capture_stderr = true;
  options.timeout = std::chrono::seconds(20);
  unset_wrapper_environment(options);
  calls = write("bootstrap-calls", "");
  options.env = {
   {"PATH", docker.parent_path().string() + ':' + support::env("PATH")}, {"PWD", checkout.string()}, {"MMLTK_DOCKER_CONTAINER", "host"}, {"MMLTK_BOOTSTRAP_CAPTURED", "1"},
   {"MMLTK_USER", std::to_string(::getuid())}, {"TEST_BOOTSTRAP_BINARY", (support::executable_path().parent_path() / "mmltk").string()}, {"TEST_BOOTSTRAP_ROOT", root.string()},
   {"TEST_BOOTSTRAP_CALLS", calls.string()}
  };
 }
 Strings bootstrap(std::string operation = "--ensure") const { return {"bash", (checkout / "tools/bootstrap/bootstrap_mmltk.sh").string(), std::move(operation), checkout.string()}; }
 Strings native(Strings arguments) const { return plus({(checkout / "native").string()}, std::move(arguments)); }
 support::ProcessResult run(const Strings& command) const { return support::run(command, options); }
 std::vector<J> invocations() const {
  std::vector<J> rows;
  for (const auto& line : support::lines(support::read_text(calls))) rows.push_back(J::parse(line));
  return rows;
 }
 void succeeded(const support::ProcessResult& result) const {
  if (result.exit_code) throw Failure(result.err);
 }
 void initialize() const { succeeded(run(bootstrap())); }
 void require_unavailable_diagnostic() const {
  const auto result = run(plus(bootstrap("--launch"), {"--logs", "--help"}));
  EQ(result.exit_code, 1);
  HAS(result.err, "diagnostic did not build or pull");
 }
 support::RunOptions holding_options(const std::string& stage, const fs::path& ready) const {
  auto settings = options;
  settings.env["TEST_TOOLS_HOLD"] = stage;
  settings.env["TEST_TOOLS_READY"] = ready.string();
  return settings;
 }
 void release(Child& child) const {
  const std::string text = "continue\n";
  EQ(::write(child.input.get(), text.data(), text.size()), static_cast<ssize_t>(text.size()));
 }
 void complete_queued(Child& owner, Child& waiter) const {
  release(owner);
  EQ(owner.wait(20), 0);
  EQ(waiter.wait(20), 0);
 }
};
void test_bootstrap_atomic_publication_concurrency_and_spaces() {
 NativeToolsFixture f;
 const auto& root = f.checkout;
 auto& options = f.options;
 const auto& calls = f.calls;
 const auto command = f.bootstrap();
 f.require_unavailable_diagnostic();
 CHECK(support::read_text(calls).empty());
 auto cold = support::run(command, options);
 if (cold.exit_code) throw Failure(cold.err);
 const auto first = fs::read_symlink(root / "build/tools");
 auto forwarded = support::run({(root / "mmltk").string(), "--tool", "runtime-package", "--", "inventory"}, options);
 EQ(forwarded.exit_code, 0);
 HAS(forwarded.out, "lib/gcc-16.2");
 EQ(support::run(command, options).exit_code, 0);
 EQ(support::lines(support::read_text(calls)).size(), 1U);
 support::write_text(root / "tools/input.cpp", "changed\n");
 f.require_unavailable_diagnostic();
 EQ(support::lines(support::read_text(calls)).size(), 1U);
 options.env["TEST_BOOTSTRAP_FAILURE"] = "1";
 EQ(support::run(command, options).exit_code, 43);
 EQ(fs::read_symlink(root / "build/tools"), first);
 options.env["TEST_BOOTSTRAP_FAILURE"] = "0";
 Child left(command, options, f.root / "left.log"), right(command, options, f.root / "right.log");
 EQ(left.wait(20), 0);
 EQ(right.wait(20), 0);
 NE(fs::read_symlink(root / "build/tools"), first);
 CHECK(fs::exists(root / "build" / first / "mmltk"));
 EQ(support::lines(support::read_text(calls)).size(), 3U);
 const auto published = fs::read_symlink(root / "build/tools");
 support::write_text(root / "tools/input.cpp", "interrupted generation\n");
 const auto ready = f.write("bootstrap-ready", "");
 options.env["TEST_BOOTSTRAP_READY"] = ready.string();
 Child cancelled(plus(f.bootstrap("--launch"), {"--tool", "runtime-package", "--", "inventory"}), options, f.root / "cancelled.log");
 wait_ready_file(ready);
 const auto client = static_cast<pid_t>(std::stoi(support::read_text(ready)));
 EQ(::kill(cancelled.pid(), SIGTERM), 0);
 EQ(cancelled.wait(10), 143);
 EQ(fs::read_symlink(root / "build/tools"), published);
 EQ(::kill(client, 0), -1);
 EQ(errno, ESRCH);
 EQ(std::distance(fs::directory_iterator(root / "build/.tools-generations"), fs::directory_iterator{}), 2);
}
void test_tools_tidy_waits_for_bootstrap_database_publication() {
 NativeToolsFixture f;
 f.initialize();
 const auto prior = fs::read_symlink(f.checkout / "build/tools");
 const auto ready = f.write("build-ready", "");
 auto settings = f.holding_options("build", ready);
 Child build(f.bootstrap("--build"), settings, f.root / "build.log", true);
 wait_ready_file(ready);
 EQ(support::read_text(f.checkout / ".cache/cmake/tools/compile_commands.json"), "[");
 FileNotifications admission(f.checkout / ".cache/locks/tools.lock", IN_OPEN);
 Child tidy(f.native({"--tidy-tools", "--file", "tools/input.cpp"}), f.options, f.root / "tidy.log");
 admission.wait();
 EQ(f.invocations().size(), 2U);
 FALSE(tidy.poll());
 f.complete_queued(build, tidy);
 EQ(f.invocations().back()["kind"], "tidy");
 NE(fs::read_symlink(f.checkout / "build/tools"), prior);
 CHECK(fs::exists(f.checkout / "build" / prior / "mmltk"));
 CHECK(fs::is_regular_file(f.checkout / ".cache/cmake/tools/tidy-object"));
}
void test_bootstrap_waits_for_tidy_recomputes_inputs_and_shares_cache_subroots() {
 for (const auto* cache : {".cache", ".cache/alternate"}) {
  NativeToolsFixture f;
  f.initialize();
  const auto ready = f.write("tidy-ready", ""), queued = f.write("build-waiting", "");
  auto settings = f.holding_options("tidy", ready);
  Child tidy(f.native({"--tidy-tools"}), settings, f.root / "tidy.log", true);
  wait_ready_file(ready);
  settings = f.options;
  settings.env["MMLTK_CACHE_ROOT"] = cache;
  settings.env["TEST_TOOLS_ADMISSION"] = queued.string();
  Child build(f.bootstrap("--build"), settings, f.root / "build.log");
  wait_ready_file(queued);
  EQ(f.invocations().size(), 2U);
  // This write represents the formatter inside the held tools-stage interval.
  support::write_text(f.checkout / "tools/input.cpp", "formatted while bootstrap waited\n");
  f.complete_queued(tidy, build);
  EQ(f.invocations().back()["kind"], "build");
  EQ(f.invocations().back()["cache"], (f.checkout / cache / "cmake/tools").string());
  const auto published = fs::read_symlink(f.checkout / "build/tools");
  f.options.env["MMLTK_CACHE_ROOT"] = cache;
  f.initialize();
  EQ(f.invocations().size(), 3U);
  EQ(fs::read_symlink(f.checkout / "build/tools"), published);
  f.succeeded(f.run(f.native({"--build-tools"})));
  EQ(f.invocations().size(), 4U);
  f.succeeded(f.run(f.native({"--tidy-tools", "--file", "tools/input.cpp"})));
  CHECK(fs::exists(f.checkout / ".cache/cmake/tools/tidy-object"));
  CHECK(fs::exists(f.checkout / cache / "cmake/tools/tidy-object"));
 }
}
void test_tools_lock_wait_cancellation_and_failure_release() {
 NativeToolsFixture f;
 f.initialize();
 const auto prior = fs::read_symlink(f.checkout / "build/tools");
 const auto ready = f.write("writer-ready", ""), queued = f.write("waiter-ready", "");
 auto settings = f.holding_options("build", ready);
 settings.env["TEST_TOOLS_FAILURE"] = "build";
 Child writer(f.bootstrap("--build"), settings, f.root / "writer.log", true);
 wait_ready_file(ready);
 settings = f.options;
 settings.env["TEST_TOOLS_ADMISSION"] = queued.string();
 Child bootstrap(f.bootstrap("--build"), settings, f.root / "bootstrap-wait.log");
 wait_ready_file(queued);
 EQ(::kill(bootstrap.pid(), SIGTERM), 0);
 EQ(bootstrap.wait(10), 143);
 {
  FileNotifications admission(f.checkout / ".cache/locks/tools.lock", IN_OPEN);
  Child tidy(f.native({"--tidy-tools", "--file", "tools/input.cpp"}), f.options, f.root / "tidy-wait.log");
  admission.wait();
  EQ(::kill(tidy.pid(), SIGTERM), 0);
  EQ(tidy.wait(10), 143);
 }
 EQ(f.invocations().size(), 2U);
 f.release(writer);
 EQ(writer.wait(20), 43);
 EQ(fs::read_symlink(f.checkout / "build/tools"), prior);
 f.succeeded(f.run(plus({(f.checkout / "build/tools/mmltk").string()}, {"--tool", "runtime-package", "--", "inventory"})));
 f.succeeded(f.run(f.native({"--build-tools"})));
 const auto published = fs::read_symlink(f.checkout / "build/tools");
 f.options.env["TEST_TOOLS_FAILURE"] = "tidy";
 EQ(f.run(f.native({"--tidy-tools"})).exit_code, 43);
 EQ(fs::read_symlink(f.checkout / "build/tools"), published);
 f.options.env.erase("TEST_TOOLS_FAILURE");
 f.succeeded(f.run(f.native({"--build-tools"})));
 const auto before_cancel = fs::read_symlink(f.checkout / "build/tools");
 support::write_text(ready, "");
 support::write_text(queued, "");
 const auto cleanup = f.write("cleanup-ready", "");
 settings = f.holding_options("tidy", ready);
 settings.env["TEST_TOOLS_CLEANUP_READY"] = cleanup.string();
 Child cancelled(f.native({"--tidy-tools"}), settings, f.root / "tidy-cancel.log", true);
 wait_ready_file(ready);
 EQ(::kill(cancelled.pid(), SIGTERM), 0);
 wait_ready_file(cleanup);
 const auto count = f.invocations().size();
 settings = f.options;
 settings.env["TEST_TOOLS_ADMISSION"] = queued.string();
 Child after_cancel(f.bootstrap("--build"), settings, f.root / "after-cancel.log");
 wait_ready_file(queued);
 EQ(f.invocations().size(), count);
 EQ(fs::read_symlink(f.checkout / "build/tools"), before_cancel);
 f.release(cancelled);
 EQ(cancelled.wait(20), 143);
 EQ(after_cancel.wait(20), 0);
}
int tools_handoff_state() {
 const fs::path root = support::env("TEST_BOOTSTRAP_ROOT");
 check_tools_handoff_consumed(root);
 auto lock = tools_lock(root);
 EQ(::flock(lock.get(), LOCK_EX | LOCK_NB), 0);
 FALSE(fs::exists(root / ".cache/locks/checkout.lock"));
 return 0;
}
void test_tools_cold_root_ownership_handoff() {
 EQ(::getuid(), 0U);
 const auto* selected = ::getpwnam("nobody");
 CHECK(selected != nullptr);
 NE(selected->pw_uid, 0U);
 const auto uid = selected->pw_uid;
 const auto gid = selected->pw_gid;
 const auto identity = std::to_string(uid) + ':' + std::to_string(gid);
 const auto permission = fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec;
 for (const bool internal : {false, true}) {
  NativeToolsFixture f;
  fs::permissions(f.root, permission);
  EQ(::chown(f.checkout.c_str(), uid, gid), 0);
  fs::permissions(f.checkout / "mmltk", permission);
  fs::permissions(f.checkout / "tools/bootstrap/bootstrap_mmltk.sh", permission);
  fs::permissions(f.calls, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::group_write | fs::perms::others_read | fs::perms::others_write);
  f.options.env["MMLTK_USER"] = internal ? "nobody" : std::to_string(uid);
  f.options.env["MMLTK_CACHE_ROOT"] = ".cache/selected/tools-owner";
  const auto cache = f.checkout / ".cache/selected/tools-owner/cmake/tools";
  const auto launch = plus(f.bootstrap("--launch"), internal ? Strings{"--tool", "test-log-query", "--", "--fixture-tools-state"} : Strings{"--tidy-tools"});
  f.succeeded(f.run(launch));
  auto owner = [&](const fs::path& path, uid_t expected_uid, gid_t expected_gid) {
   struct stat metadata{};
   EQ(::lstat(path.c_str(), &metadata), 0);
   EQ(metadata.st_uid, expected_uid);
   EQ(metadata.st_gid, expected_gid);
   return metadata.st_mode & 0777;
  };
  EQ(owner(f.checkout / ".cache/locks/tools.lock", uid, gid), 0664U);
  for (const auto& path : {cache, f.checkout / "build", f.checkout / "build/.tools-generations", f.checkout / "build/tools"}) owner(path, uid, gid);
  for (const auto& entry : fs::recursive_directory_iterator(cache)) owner(entry.path(), uid, gid);
  const auto generation = fs::read_symlink(f.checkout / "build/tools");
  for (const auto& entry : fs::recursive_directory_iterator(f.checkout / "build" / generation)) owner(entry.path(), uid, gid);
  // Native repair is confined to tools resources, including in a cache subroot.
  owner(f.checkout / "tools/input.cpp", 0, ::getgid());
  if (internal) {
   FALSE(fs::exists(f.checkout / ".cache/locks/checkout.lock"));
   f.succeeded(f.run(f.native({"--tidy-tools"})));
  }
  EQ(f.invocations().front()["user"], "0:" + std::to_string(::getgid()));
  EQ(f.invocations().back()["kind"], "tidy");
  EQ(f.invocations().back()["user"], identity);
  CHECK(fs::exists(cache / "tidy-object"));
  f.succeeded(f.run(f.native({"--build-tools"})));
  EQ(f.invocations().back()["user"], identity);
  const auto before_nonroot = fs::read_symlink(f.checkout / "build/tools");
  auto unprivileged = support::command::self_invocation(support::command::Command::test_log_query, plus({"--fixture-tools-user", identity}, {(f.checkout / "native").string(), "--build-tools"}));
  f.succeeded(f.run(unprivileged));
  EQ(f.invocations().back()["user"], identity);
  NE(fs::read_symlink(f.checkout / "build/tools"), before_nonroot);
  CHECK(fs::exists(f.checkout / "build" / generation / "mmltk"));
  // A warm read-only invocation does not repair ownership or rebuild tools.
  const auto lock_path = f.checkout / ".cache/locks/tools.lock";
  EQ(::chown(lock_path.c_str(), 0, ::getgid()), 0);
  EQ(::chmod(lock_path.c_str(), 0600), 0);
  const auto count = f.invocations().size();
  f.succeeded(f.run({(f.checkout / "mmltk").string(), "--logs", "--help"}));
  EQ(f.invocations().size(), count);
  EQ(owner(lock_path, 0, ::getgid()), 0600U);
  f.options.env["MMLTK_USER"] = "mmltk-user-that-does-not-exist";
  f.succeeded(f.run({(f.checkout / "mmltk").string(), "--tool", "runtime-package", "--", "inventory"}));
 }
}
void test_tools_ownership_selection_keeps_normal_fixture_restrictions() {
 WrapperFixture f(R"SH(#!/bin/sh
case "$1" in
 version|image) printf 'fixture-image\n';;
 run) printf '%s\n' "$@" >"${TEST_TOOLS_CONTAINER_ARGS}";;
esac
)SH");
 const auto calls = f.write("container-arguments", "");
 f.options.env["TEST_TOOLS_CONTAINER_ARGS"] = calls.string();
 for (const bool ownership : {false, true}) {
  const auto result = f.invoke({"--test", "log-query-tool", ownership ? "--tools-ownership" : "--list-tests"});
  if (result.exit_code) throw Failure(result.err);
  const auto arguments = support::lines(support::read_text(calls));
  auto option = [&](std::string_view name) {
   const auto found = std::find(arguments.begin(), arguments.end(), name);
   CHECK(found != arguments.end() && std::next(found) != arguments.end());
   return *std::next(found);
  };
  EQ(option("--cap-drop"), "ALL");
  EQ(option("--security-opt"), "no-new-privileges");
  EQ(option("--network"), "none");
  EQ(option("--volume"), f.root.string() + ":/workspace:ro");
  CHECK(std::find(arguments.begin(), arguments.end(), "--read-only") != arguments.end());
  EQ(option("--tmpfs"), "/tmp:rw,exec,nosuid,nodev,size=32m");
  Strings capabilities;
  for (auto found = arguments.begin(); found != arguments.end(); ++found)
   if (*found == "--cap-add") capabilities.push_back(*++found);
  EQ(capabilities, ownership ? Strings({"CHOWN", "DAC_OVERRIDE", "FOWNER", "SETUID", "SETGID", "KILL"}) : Strings{});
  if (ownership) EQ(option("--user"), "0:0");
  EQ(arguments.back(), ownership ? "--tools-ownership" : "--list-tests");
 }
 auto ordinary = support::capture(support::command::self_invocation(support::command::Command::test_log_query, {"NativeToolTests.test_tools_cold_root_ownership_handoff"}));
 EQ(ordinary.exit_code, 2);
 HAS(ordinary.err, "--tools-ownership");
}
void family_fixture(FileFixture& f) {
 f.write("build/validation/session.jsonl", j(R"json([{"event":"browser.server.started","steady_ns":100},{"event":"child.signaled","steady_ns":200,"value":139}])json"));
 f.write("build/validation/session-native.log", "native plain\n");
 f.write("build/validation/session-firefox.log", j(R"json([{"event":"firefox.webgpu.blocking_map","detail":"arc_extract","buffer":"BufferId(21,1)","strong_count":2,"settled":false}])json"));
 f.write("build/validation/session.jsonl.history/57-1000000000.jsonl", j(R"json([{"event":"browser.server.started","steady_ns":10},{"event":"child.signaled","steady_ns":20,"value":143}])json"));
 f.write("build/validation/session-native.log.history/57-1000030000.log", "older native\n");
 f.write("build/validation/session-firefox.log.history/57-1000060000.log", j(R"json([{"event":"firefox.webgpu.blocking_map","detail":"settled","buffer":"BufferId(21,1)"}])json"));
}
void test_family_and_history_pairing_retains_inference_and_archive_identity() {
 FileFixture f;
 family_fixture(f);
 auto result = f.exported({"--family", "session", "--run", "57-1000030000"});
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/session@57-1000000000"});
 EQ(column_set(result.rows, "_log", "run_link"), std::set<J>{"rotation-neighbor-within-10ms"});
 EQ(column_set(result.rows, "_log", "file").size(), 3U);
 CHECK(all(result.rows, [](auto& r) { return r["_log"].contains("archive_id"); }));
 auto listing = f.query({"--family", "session", "--history", "--list-runs"});
 EQ(listing.status, 0);
 includes(listing.out, {"session@57-1000000000", "session@current", "links are inferred"});
}
void test_family_discovers_separate_acceptance_and_rotating_mozilla_evidence() {
 FileFixture f;
 family_fixture(f);
 f.write("build/validation/session-acceptance.jsonl", j(R"json([{"event":"acceptance.readiness.completed"}])json"));
 f.write("build/validation/session-application.log", "application text\n");
 for (auto [role, pid, child] : std::vector<std::tuple<std::string, int, std::string>>{{"main", 12, ""}, {"child", 13, ".child-4"}})
  for (int index = 0; index < 4; ++index)
   f.write(
    "build/validation/session-mozilla-" + role + '.' + std::to_string(pid) + ".log" + child + ".moz_log." + std::to_string(index), "Mozilla module " + role + " ring " + std::to_string(index) + '\n');
 f.write("build/validation/session-acceptance.jsonl.history/57-1000070000.jsonl", j(R"json([{"event":"acceptance.shutdown.stalled"}])json"));
 f.write("build/validation/session-application.log.history/57-1000075000.log", "old application\n");
 f.write("build/validation/session-mozilla-child.9.log.child-3.moz_log.0.history/57-1000080000.log", "old Mozilla\n");
 auto result = f.exported({"--family", "session"});
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "file").size(), 13U);
 EQ(column_set(result.rows, "_log", "role"), (std::set<J>{"trace", "native", "firefox", "acceptance", "application", "mozilla"}));
 result = f.exported({"--family", "session", "--run", "57-1000080000"});
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "file").size(), 6U);
 EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/session@57-1000000000"});
 result = f.exported({"build/validation"});
 EQ(result.status, 0);
 EQ(select(result.rows, [](auto& r) { return r["_log"]["role"] == "mozilla"; }).size(), 8U);
}
logs::ArtifactCatalog catalog_with_paths(const logs::Options& options, const fs::path& root, const std::set<fs::path>& expected) {
 logs::ArtifactCatalog catalog(options, root);
 EQ(projected_set(catalog.files, [](auto& file) { return file.path; }), expected);
 return catalog;
}
std::pair<fs::path, fs::path> family_archive(FileFixture& fixture, const std::string& family, const std::string& suffix, std::set<fs::path>& expected) {
 const auto current = fixture.write("build/validation/" + family + suffix, "");
 const auto archive = fixture.write(current.lexically_relative(fixture.root).string() + ".history/57-1000000000" + (suffix == ".jsonl" ? ".jsonl" : ".log"), "");
 expected.insert(current);
 expected.insert(archive);
 return {current, archive};
}
void test_family_inventory_groups_selectors_without_capturing_unrelated_entries() {
 FileFixture f;
 std::set<fs::path> expected, histories;
 for (const std::string family : {"first", "second", "third"})
  for (const std::string suffix : {".jsonl", "-application.1.log", "-mozilla-child.13.log.child-4.child-5.moz_log.2"}) {
   const auto [current, archive] = family_archive(f, family, suffix, expected);
   histories.insert(archive.parent_path());
  }
 for (int i = 0; i < 80; ++i) {
  f.write("build/validation/unrelated-" + std::to_string(i) + ".bin", "");
  f.write("build/validation/other-mozilla-main." + std::to_string(i) + ".log.moz_log.0", "");
 }
 auto options = logs::parse_options({"--family", "first", "--family", "second", "--family", "third", "--family", "first", "--family", "build/validation/./second.jsonl", "--history", "--triage"});
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 128);
  Observe observed;
  auto catalog = catalog_with_paths(options, f.root, expected);
  std::map<fs::path, std::size_t> counts{{f.root / "build/validation", 1}};
  for (auto& p : histories) counts[p] = 1;
  EQ(path_counts(observed, "directory.open"), counts);
  EQ(observed.count("file.capture"), expected.size());
  EQ(observed.maximum("catalog.candidate"), 86U);
  EQ(observed.count("catalog.candidate"), 86U);
 }
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 85);
  raises([&] { logs::ArtifactCatalog catalog(options, f.root); }, "candidate inventory exceeds file budget");
 }
}
void test_family_symlink_reuses_earlier_target_candidates_and_checks_containment() {
 FileFixture f;
 std::set<fs::path> expected;
 const std::string process = "-mozilla-child.12.log.child-3.moz_log.0";
 for (const auto& suffix : Strings{".jsonl", "-application.1.log", process}) { family_archive(f, "a-target", suffix, expected); }
 auto parent = f.root / "build/validation", alias = parent / ("z-alias" + process);
 fs::create_symlink(parent / ("a-target" + process), alias);
 auto options = logs::parse_options({"--family", "z-alias", "--history", "--triage"});
 {
  Observe observed;
  auto catalog = catalog_with_paths(options, f.root, expected);
  EQ(projected_set(catalog.files, [](auto& s) { return s.metadata["family"]; }), std::set<J>{"build/validation/a-target"});
  EQ(observed.count("file.capture"), expected.size());
  std::map<fs::path, std::size_t> counts{{parent, 1}};
  for (auto& p : expected)
   if (p.parent_path() != parent) counts[p.parent_path()] = 1;
  EQ(path_counts(observed, "directory.open"), counts);
  EQ(path_counts(observed, "directory.close"), counts);
  auto target_entry =
   std::find_if(observed.events.begin(), observed.events.end(), [&](const auto& e) { return e.event == "directory.entry" && e.value == (parent / ("a-target" + process)).string(); });
  auto alias_capture = std::find_if(observed.events.begin(), observed.events.end(), [&](const auto& e) { return e.event == "file.capture" && e.value == alias.string(); });
  // Native discovery inventories a directory before capturing its
  // aliases, so the demanding reuse ordering is independent of readdir.
  CHECK(target_entry != observed.events.end());
  CHECK(alias_capture != observed.events.end());
  CHECK(target_entry < alias_capture);
 }
 support::TemporaryDirectory outside;
 auto external = outside.path() / ("escaped" + process);
 support::write_text(external, "outside\n");
 fs::remove(alias);
 fs::create_symlink(external, alias);
 raises([&] { logs::ArtifactCatalog catalog(options, f.root); }, "escapes repository");
}
void test_child_mozilla_archived_only_family_and_direct_role() {
 FileFixture f;
 const std::string archive = "build/validation/retired-mozilla-child.34.log.child-7.moz_log.2.history/57-1000000000.log";
 f.write(archive, "archived child module\n");
 f.write("build/validation/retired.jsonl.history/57-1000000000.jsonl", j(R"json([{"event":"shutdown.complete"}])json"));
 for (auto args : std::vector<Strings>{{"--family", "retired", "--history"}, {"--family", "retired", "--run", "57-1000000000"}}) {
  auto result = f.exported(args);
  EQ(result.status, 0);
  EQ(column_set(result.rows, "_log", "role"), (std::set<J>{"trace", "mozilla"}));
  EQ(column_set(result.rows, "_log", "family"), std::set<J>{"build/validation/retired"});
  EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/retired@57-1000000000"});
 }
 auto result = f.exported({archive});
 EQ(result.status, 0);
 EQ(result.rows[0]["_log"]["role"], J("mozilla"));
 for (auto name : {"retired-mozilla-main.3.log.moz_log", "retired-mozilla-child.4.log.child-8.moz_log.3", "retired-mozilla-child.5.log.child-8.child-9.moz_log.0"}) {
  auto match = logs::match(logs::MOZILLA_ARTIFACT, name);
  CHECK(match);
  EQ(match[1], "retired");
 }
 for (auto name :
  {"retired-mozilla-child.4.log.moz_log.0", "retired-mozilla-main.3.log.child-8.moz_log.0", "retired-mozilla-child.4.log.child-x.moz_log.0", "retired-mozilla-child.4.log.child-8.moz_log.4",
   "unrelated.moz_log.0"})
  FALSE(logs::full(logs::MOZILLA_ARTIFACT, name));
}
void test_mozilla_family_retains_containment_and_discovery_budget() {
 FileFixture f;
 f.write("build/validation/session.jsonl", j(R"json([{"event":"native"}])json"));
 f.write("build/validation/session-mozilla-main.1.log.moz_log.0", "Mozilla\n");
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 6);
  auto result = f.query({"--family", "session"});
  EQ(result.status, 2);
  HAS(result.err, "file budget");
 }
 support::TemporaryDirectory outside;
 auto target = outside.path() / "escaped.log";
 support::write_text(target, "outside\n");
 fs::create_symlink(target, f.root / "build/validation/session-mozilla-child.2.log.child-6.moz_log.0");
 auto result = f.query({"--family", "session"});
 EQ(result.status, 2);
 HAS(result.err, "escapes repository");
}
void test_history_catalog_enumerates_each_archive_directory_once() {
 FileFixture f;
 for (const std::string suffix : {".jsonl", "-native.log", "-firefox.log"}) {
  auto current = f.write("build/validation/capture" + suffix, "");
  for (int i = 0; i < 12; ++i) f.write(current.lexically_relative(f.root).string() + ".history/57-" + std::to_string(i * 20000000) + current.extension().string(), "");
 }
 auto options = logs::parse_options({"build/validation/capture.jsonl.history/*.jsonl", "--history", "--triage"});
 std::set<fs::path> original;
 {
  Observe observed;
  logs::ArtifactCatalog catalog(options, f.root);
  EQ(catalog.files.size(), 36U);
  original = projected_set(catalog.files, [](auto& s) { return s.path; });
  auto counts = path_counts(observed, "directory.logs");
  EQ(counts.size(), 3U);
  CHECK(all(counts, [](auto& p) { return p.second == 1; }));
  std::map<fs::path, std::size_t> physical{
   {f.root / "build/validation", 1}, {f.root / "build/validation/capture.jsonl.history", 1}, {f.root / "build/validation/capture-native.log.history", 1},
   {f.root / "build/validation/capture-firefox.log.history", 1}
  };
  EQ(path_counts(observed, "directory.open"), physical);
  EQ(path_counts(observed, "directory.close"), physical);
  // libc owns wildcard enumeration. Observe its actual invocation in
  // addition to the tool's one full traversal of each history directory.
  EQ(select(observed.events, [](auto& e) { return e.event == "glob.expand" && e.value.template get<std::string>().ends_with("capture.jsonl.history/*.jsonl"); }).size(), 1U);
  EQ(observed.count("file.capture"), 36U);
 }
 for (auto direct : {"build/validation/capture.jsonl.history", "/host/repo/build/validation/capture.jsonl.history"}) {
  auto configured = logs::parse_options({direct, "--history", "--triage"});
  Environment mapped("MMLTK_LOG_HOST_ROOT", "/host/repo");
  Observe observed;
  auto catalog = catalog_with_paths(configured, f.root, original);
  auto physical = path_counts(observed, "directory.open");
  CHECK(all(physical, [](auto& p) { return p.second == 1; }));
  EQ(physical.size(), 4U);
  EQ(observed.count("file.capture"), 36U);
 }
}
void test_shared_event_anchors_link_transcript_and_propagate_test_tags() {
 FileFixture f;
 family_fixture(f);
 for (const std::string name : {"transcript.log", "session-acceptance.log"}) {
  auto path = "build/validation/" + name;
  f.write(path,
   "Filters: [viewer_copy][hardware]\n[ RUN ] workspace_wayland_viewer_copy\nworkspace-wayland[native]: "
   "{\"event\":\"browser.server.started\",\"steady_ns\":100}\n{\"event\":\"firefox.webgpu.blocking_map\",\"buffer\":\"BufferId(21,1)\",\"detail\":\"arc_extract\"}\nnative host terminal status: exit "
   "139\n[ FAILED ] workspace_wayland_viewer_copy\n");
  auto result = f.exported({path, "--family", "session", "-q", "@tags:viewer_copy", "--limit", "30"});
  EQ(result.status, 0);
  auto related = select(result.rows, [](auto& r) { return r["_log"]["file"].template get<std::string>().ends_with("session.jsonl"); });
  EQ(related.size(), 2U);
  CHECK(all(related, [](auto& r) { return r["_log"]["test"] == "workspace_wayland_viewer_copy"; }));
  auto terminal = select(result.rows, [](auto& r) { return r["data"].value("event", "") == "process.terminal"; });
  CHECK(terminal);
  EQ(terminal[0]["_log"]["run"], J("build/validation/session@current"));
  EQ(terminal[0]["_log"]["run_link"], J("shared-event-and-steady-time"));
 }
}
void test_related_run_includes_terminal_and_never_crosses_reused_buffer_ids() {
 FileFixture f;
 family_fixture(f);
 auto result = f.exported({"--family", "session", "--history", "-q", "detail=arc_extract", "--correlate", "buffer", "--related-run", "--limit", "30"});
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/session@current"});
 CHECK(any(result.rows, [](auto& r) { return r["data"].value("event", "") == "child.signaled"; }));
 auto report = f.query({"--family", "session", "-q", "buffer=\"BufferId(21,1)\"", "--limit", "1"});
 EQ(report.status, 0);
 includes(report.out, {"Terminals in matched runs", "@signal=SIGSEGV", "@exit_code=139"});
}
void test_transcript_incident_keeps_buffer_ids_failure_and_catch_context() {
 FileFixture f;
 f.write("incident.log",
  "Filters: [viewer_copy]\n[ RUN ] "
  "workspace_wayland_viewer_copy\n{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"arc_extract\",\"buffer\":\"BufferId(21,1)\",\"strong_count\":2,\"settled\":false}\n/workspace/test.cpp:4181: "
  "failed: cleanup_count >= 2 with 2 messages: 'termination: sigint' and 'native diagnostics: "
  "eration\":0,\"trace_id\":721\n{\"event\":\"child.signaled\",\"steady_ns\":131858036752003,\"value\":139}\n' and 'Firefox diagnostics: "
  "{\"event\":\"firefox.webgpu.blocking_map\",\"buffer\":\"BufferId(21,1)\",\"detail\":\"callback\"}'\n[ FAILED ] workspace_wayland_viewer_copy\n");
 auto result = f.query({"incident.log", "-q", "viewer_copy", "--limit", "20"});
 EQ(result.status, 0);
 HAS(f.query({"incident.log", "-q", "buffer=\"BufferId(21,1)\"", "--format", "jsonl"}).out, "strong_count");
 includes(result.out, {"catch.assertion_failed", "@signal=SIGSEGV", "context-copy"});
 LACKS(result.err, "Traceback");
}
void set_mtime(const fs::path& path, std::int64_t ns) {
 timespec time{ns / 1000000000, ns % 1000000000};
 const std::array<timespec, 2> times{time, time};
 CHECK(::utimensat(AT_FDCWD, path.c_str(), times.data(), 0) == 0);
}
std::string pasted_error_fixture(FileFixture& f) {
 const std::string message = "Presentation unavailable: Explore requires a measured non-empty gallery.";
 for (const std::string suffix : {".jsonl", "-native.log", "-firefox.log"}) f.write("build/validation/presentation" + suffix, "");
 auto native = f.write("build/validation/presentation.jsonl.history/57-2000000000.jsonl",
  j(
   R"json([{"event":"browser.server.started","steady_ns":1000000000},{"event":"browser.intent.accepted","steady_ns":1001000000,"sequence":107},{"event":"browser.reply.sent","steady_ns":1002000000,"sequence":107},{"event":"child.signaled","steady_ns":1021000000,"value":143}])json"));
 auto runtime = f.write("build/validation/presentation-native.log.history/57-2000030000.log", "1970-01-01 00:00:01.010 [1:2] [host] [debug] event=explore.completed\n");
 auto firefox = f.write("build/validation/presentation-firefox.log.history/57-2000060000.log",
  J::array(
   {{{"event", "integration.explore_message"}, {"detail", "Gallery(Measured { width: 896 })"}, {"elapsed_ms", 19320}},
    {{"event", "integration.explore_message"}, {"detail", "Detail(CloseRequested)"}, {"elapsed_ms", 19435}},
    {{"event", "integration.explore_message"}, {"detail", "Dataset(OpenRequested)"}, {"elapsed_ms", 19478}}, {{"event", "integration.failed"}, {"detail", message}, {"elapsed_ms", 19479}},
    {{"event", "integration.surface_sync"}, {"elapsed_ms", 19500}}}));
 for (auto& path : {native, runtime, firefox}) set_mtime(path, INT64_C(1700000019500000000));
 auto later = f.write("build/validation/presentation-firefox.log.history/57-3000060000.log", J::array({{{"event", "integration.failed"}, {"detail", message}, {"elapsed_ms", 20557}}}));
 set_mtime(later, INT64_C(1700000099500000000));
 f.write("build/validation/acceptance.log",
  "Filters: [viewer_copy]\n[ RUN ] workspace_wayland_viewer_copy\nworkspace-wayland[native]: {\"event\":\"browser.server.started\",\"steady_ns\":1000000000}\nworkspace-wayland[firefox]: " +
   fixture_json(J{{"event", "integration.failed"}, {"detail", message}, {"elapsed_ms", 19479}}) + "\n[ FAILED ] workspace_wayland_viewer_copy\n");
 return "Presentation unavailable\n\nExplore requires a measured non-empty gallery.";
}
std::string first_match_line(const std::string& output) {
 auto lines = select(support::lines(output), [](auto& l) { return l.starts_with("First physical match:"); });
 CHECK(lines);
 return lines[0];
}
void test_pasted_multiline_error_finds_earliest_physical_record_and_nearby_family() {
 FileFixture f;
 auto pasted = pasted_error_fixture(f);
 auto result = f.query({"--error", pasted});
 EQ(result.status, 0);
 includes(result.out, {"exact/display-normalized phrase", "3 occurrences", "Gallery(Measured", "Detail(CloseRequested)", "Dataset(OpenRequested)", "integration.failed", "browser.intent.accepted",
                       "browser.reply.sent", "SIGTERM", "file-mtime proximity", "same-clock proximity"});
 includes(first_match_line(result.out), {"57-2000060000.log:4", "19479.000ms"});
 HAS(result.err, "proximity is not causality");
 LACKS(result.out, "57-3000060000.log");
}
void test_pasted_error_uses_phrase_before_unordered_fallback() {
 FileFixture f;
 f.write("build/validation/example.log", "gallery empty because unavailable presentation\npresentation unavailable empty gallery\n");
 auto result = f.query({"--error", "presentation unavailable empty gallery"});
 EQ(result.status, 0);
 HAS(result.out, "exact/display-normalized phrase; 1 occurrences");
 HAS(first_match_line(result.out), "example.log:2");
 result = f.query({"--error", "unavailable empty presentation gallery"});
 EQ(result.status, 0);
 HAS(result.out, "all-words fallback; 2 occurrences");
}
void test_pasted_error_selection_scans_once_with_exact_fallback_and_unicode() {
 FileFixture f;
 f.write("input.jsonl", "{\"message\":\"blue red\"}\n{\"message\":\"red blue\"}\n{\"message\":\"\\u0072ed \\u0062lue\"}\n{\"broken\":\n");
 auto files = discover({"input.jsonl"}, f.root);
 logs::Expression query;
 for (bool strict : {false, true}) {
  auto options = logs::parse_options(strict ? Strings{"--strict"} : Strings{});
  for (auto [text, mode, count, line] : std::vector<std::tuple<std::string, std::string, std::size_t, std::optional<std::size_t>>>{
        {"red blue", "exact/display-normalized phrase", 2, 2}, {"red, blue", "all-words fallback", 3, 1}, {"unseen message", "", 0, {}}, {"!!!", "", 0, {}}
       }) {
   logs::ErrorLookup lookup(text, options);
   Observe observed;
   lookup.locate(files, query, query, {});
   EQ(observed.count("file.records"), 1U);
   CHECK(observed.count("record.text") <= 4);
   EQ(lookup.mode, mode);
   EQ(lookup.matches, count);
   EQ(lookup.focus ? std::optional<std::size_t>(lookup.focus->line) : std::nullopt, line);
   EQ(lookup.malformed, static_cast<std::size_t>(strict || text == "!!!"));
  }
 }
}
void test_error_neighborhood_respects_radius_and_machine_output_bounds() {
 FileFixture f;
 auto pasted = pasted_error_fixture(f);
 auto result = f.exported({"--error", pasted, "--near-ms", "50", "--limit", "5"});
 EQ(result.status, 0);
 CHECK(result.rows.size() <= 5);
 CHECK(any(result.rows, [](auto& r) { return r["data"].value("event", "") == "integration.failed"; }));
 FALSE(any(result.rows, [](auto& r) { return contains(r["data"].value("detail", ""), "Gallery(Measured"); }));
 CHECK(all(result.rows, [](auto& r) { return r["_log"].contains("proximity_ns"); }));
}
void test_error_lookup_reports_absence_and_rejects_empty_text() {
 FileFixture f;
 f.write("build/validation/example.log", "ordinary success\n");
 EQ(f.query({"--error", "not present anywhere"}).status, 1);
 EQ(f.query({"--error", " \n "}).status, 2);
 f.write("build/validation/broken.jsonl", "{\"event\":");
 EQ(f.query({"--error", "not present anywhere", "--strict"}).status, 2);
}
void test_triage_identity_namespaces_exclude_ambient_empty_and_bare_counters() {
 auto sample = record(j(
  R"json({"surface_high":0,"surface_low":0,"trace_id":0,"span_id":true,"device_id":"DeviceId(0,1)","pid":20,"thread_id":3,"source_session":2,"source_instance":4,"sequence":18,"generation":1,"texture_id":"TextureId(2, 1)","buffer":"BufferId(21,1)","request_id":"abc","parent_span_id":52,"user_id":"user","empty_id":""})json"));
 EQ(projected_set(logs::strong_identities(sample), [](auto& id) { return id.label(); }),
  (std::set<std::string>{"source_session+source_instance=2+4", "TextureId=TextureId(2,1)", "BufferId=BufferId(21,1)", "request_id=abc", "span_id=52"}));
 CHECK(any(logs::strong_identities(sample, {{"device_id"}}), [](auto& id) { return id.names == Strings{"device_id"}; }));
 NE(logs::strong_identities(record({{"request_id", "1"}})), logs::strong_identities(record({{"request_id", 1}})));
}
std::string triage_fixture(FileFixture& f) {
 const std::string surface = "00000000000000010000000000000002";
 f.write("build/validation/triage.jsonl",
  j(
   R"json([{"event":"browser.server.started","steady_ns":100},{"event":"task.requested","surface_high":1,"surface_low":2,"steady_ns":200,"trace_id":55,"generation":9,"slot":0},{"event":"task.started","trace_id":55,"span_id":56,"steady_ns":220},{"event":"scheduler.dispatch","generation":9,"slot":0,"sequence":7,"steady_ns":230},{"event":"worker.completed","parent_span_id":56,"steady_ns":240},{"event":"task.completed","trace_id":55,"span_id":56,"steady_ns":250},{"event":"upload.started","surface_high":1,"surface_low":2,"steady_ns":260},{"event":"child.signaled","value":139,"steady_ns":300}])json"));
 f.write("build/validation/triage-firefox.log",
  J::array(
   {{{"event", "ui.measure"}, {"elapsed_ms", 90}}, {{"event", "renderer.claim_requested"}, {"surface", surface}}, {{"event", "renderer.claim_outcome"}, {"surface", surface}, {"outcome", "claimed"}},
    {{"event", "renderer.probe_failed"}, {"surface", surface}, {"boundary", "Allocation"}}, {{"event", "renderer.draw_missing"}, {"surface", surface}, {"generation", 9}, {"slot", 0}},
    {{"event", "renderer.ready"}, {"surface", surface}}, {{"event", "renderer.retired"}, {"surface", surface}}}));
 f.write("build/validation/triage-acceptance.log",
  "Filters: [pixel_probe]\n[ RUN ] workspace_pixel_probe\nworkspace-wayland[native]: {\"event\":\"browser.server.started\",\"steady_ns\":100}\n/workspace/test.cpp:44: failed: rendered probe with 1 "
  "message: 'native failure'\n[ FAILED ] workspace_pixel_probe\n");
 return surface;
}
void test_triage_automatically_correlates_probe_surface_catch_native_and_counter_leaf() {
 FileFixture f;
 auto surface = triage_fixture(f);
 Strings args = {"build/validation/triage-acceptance.log", "--family", "triage", "--triage", "--limit", "30"};
 auto result = f.query(args);
 EQ(result.status, 0);
 includes(result.out, {"renderer.probe_failed", "catch.assertion_failed", "SIGSEGV", surface, "upload", "missing-counterpart", "trace_id=55", "span_id=56", "counter refinement", "scheduler.dispatch",
                       "Earliest divergence", "not a root-cause conclusion"});
 HAS(result.err, "proximity does not establish causality");
 auto exported = f.exported(plus(args, {"--limit", "8"}));
 EQ(exported.status, 0);
 CHECK(exported.rows.size() <= 8);
 CHECK(all(exported.rows, [](auto& r) { return truth(r["_log"].value("triage_reason", J(nullptr))); }));
 CHECK(events(exported.rows).contains("renderer.probe_failed"));
 CHECK(events(exported.rows).contains("catch.assertion_failed"));
 EQ(column_set(exported.rows, "_log", "run"), std::set<J>{"build/validation/triage@current"});
}
void test_triage_single_file_discovers_current_siblings_without_expanding_explicit_queries() {
 FileFixture f;
 f.write("build/validation/capture-firefox.log", j(R"json([{"event":"renderer.probe_failed","request_id":7}])json"));
 f.write("build/validation/capture.jsonl", j(R"json([{"event":"task.started","request_id":7},{"event":"child.signaled","value":139}])json"));
 f.write("build/validation/capture-native.log", j(R"json([{"event":"acceptance.failed","request_id":7}])json"));
 const std::string path = "build/validation/capture-firefox.log";
 auto result = f.exported({path, "--triage"});
 EQ(result.status, 0);
 for (auto event : {"renderer.probe_failed", "task.started", "child.signaled", "acceptance.failed"}) CHECK(events(result.rows).contains(event));
 auto task = select(result.rows, [](auto& r) { return r["data"]["event"] == "task.started"; });
 EQ(task[0]["_log"]["discovery"], J("current artifact sibling"));
 auto probe = select(result.rows, [](auto& r) { return r["data"]["event"] == "renderer.probe_failed"; });
 FALSE(probe[0]["_log"].contains("discovery"));
 HAS(result.err, "[auto: current artifact sibling]");
 EQ(column(f.exported({path}).rows, "data", "event"), std::vector<J>{"renderer.probe_failed"});
 {
  Limit limit(mmltk::log_query::detail::limits.distinct_keys, 1);
  auto bounded = f.query({path, "--triage"});
  EQ(bounded.status, 2);
  HAS(bounded.err, "automatic artifact discovery exceeds file budget");
 }
}
void test_triage_single_archive_discovers_only_its_rotation_batch_without_current_files() {
 FileFixture f;
 for (auto [base, suffix, timestamp, event] : std::vector<std::tuple<std::string, std::string, int, std::string>>{
       {"capture", ".jsonl", 1000000, "task.started"}, {"capture-native", ".log", 1000100, "acceptance.failed"}, {"capture-firefox", ".log", 1000200, "renderer.probe_failed"},
       {"capture", ".jsonl", 1000300, "unrelated.failed"}, {"capture-native", ".log", 1000400, "unrelated.failed"}, {"capture-firefox", ".log", 1000500, "unrelated.failed"}
      }) {
  J item = {{"event", event}, {"request_id", 7}};
  if (event == "unrelated.failed") item["failed"] = true;
  f.write("build/validation/" + base + suffix + ".history/57-" + std::to_string(timestamp) + suffix, J::array({item}));
 }
 const std::string path = "build/validation/capture-firefox.log.history/57-1000200.log";
 auto result = f.exported({path, "--triage"});
 EQ(result.status, 0);
 EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/capture@57-1000000"});
 EQ(events(result.rows), (std::set<std::string>{"task.started", "acceptance.failed", "renderer.probe_failed"}));
 includes(result.err, {"[auto: adjacent rotation (inferred)]", "[rotation-neighbor-within-10ms]"});
 LACKS(result.err, "1000300");
 f.write("build/validation/capture.jsonl", j(R"json([{"event":"current.failed","failed":true}])json"));
 EQ(f.exported({path, "--triage"}).rows, result.rows);
}
void test_triage_named_archives_match_exact_names_and_reject_external_history() {
 FileFixture f;
 const std::string path = "build/validation/capture-firefox.log.history/investigation.log";
 f.write(path, j(R"json([{"event":"renderer.failed","request_id":7}])json"));
 f.write("build/validation/capture.jsonl.history/investigation.jsonl", j(R"json([{"event":"task.started","request_id":7}])json"));
 f.write("build/validation/capture.jsonl.history/other.jsonl", j(R"json([{"event":"unrelated.failed","failed":true}])json"));
 auto result = f.exported({path, "--triage"});
 EQ(result.status, 0);
 EQ(events(result.rows), (std::set<std::string>{"renderer.failed", "task.started"}));
 HAS(result.err, "[auto: matching archive name]");
 support::TemporaryDirectory external;
 fs::create_directory_symlink(external.path(), f.root / "build/validation/capture-native.log.history");
 auto error = f.query({path, "--triage"});
 EQ(error.status, 2);
 HAS(error.err, "escapes repository mount");
}
std::string between(const std::string& text, std::string_view start, std::string_view stop) {
 auto first = text.find(start);
 CHECK(first != std::string::npos);
 first += start.size();
 auto last = text.find(stop, first);
 return text.substr(first, last == std::string::npos ? last : last - first);
}
void test_triage_ranks_explicit_failure_above_error_related_events_without_changing_errors_filter() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"ui.error_modal","trace_id":7},{"event":"worker.failed"}])json"));
 auto result = f.query({"input.jsonl", "--triage", "--triage-anchors", "1"});
 EQ(result.status, 0);
 auto anchors = between(result.out, "Anchors (ranked evidence, not causes):", "Highlights:");
 HAS(anchors, "worker.failed");
 LACKS(anchors, "ui.error_modal");
 EQ(events(f.exported({"input.jsonl", "--errors"}).rows), (std::set<std::string>{"ui.error_modal", "worker.failed"}));
 result = f.query({"input.jsonl", "--triage", "-q", "event=ui.error_modal", "--where", "event=ui.error_modal"});
 HAS(result.out, "failure-related event (not an explicit failure)");
 LACKS(result.out, "anchor-failure");
}
void test_triage_finds_short_reconnect_lifetimes_without_claiming_their_cause() {
 FileFixture f;
 J rows = J::array();
 for (int peer = 1; peer < 7; ++peer) {
  rows.push_back({{"event", "browser.server.peer_opened"}, {"value", peer}, {"steady_ns", peer * INT64_C(250000000)}});
  rows.push_back({{"event", "browser.server.peer_closed"}, {"value", peer}, {"steady_ns", peer * INT64_C(250000000) + 1000000}});
 }
 f.write("input.jsonl", rows);
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 includes(result.out, {"rapid reconnects:", "cause not recorded"});
 for (int variant = 0; variant < 4; ++variant) {
  auto changed = rows;
  if (variant == 0)
   changed.erase(changed.begin() + 4, changed.end());
  else
   for (auto& r : changed) {
    if (variant == 1 && r["event"].get<std::string>().ends_with("closed"))
     r["value"] = 999;
    else if (variant == 2)
     r["steady_ns"] = r["steady_ns"].get<std::int64_t>() * 20;
    else if (variant == 3)
     r["steady_ns"] = nullptr;
   }
  f.write("input.jsonl", changed);
  LACKS(f.query({"input.jsonl", "--triage"}).out, "rapid reconnects:");
 }
}
void test_pasted_transport_error_retains_field_cause_and_reconnect_context() {
 FileFixture f;
 const std::string message =
  "invalid server CBOR record: SystemEvent system_id=7 event_id=8 state_revision=9: UpscaleChanged.snapshot: UpscaleSnapshot.scene: AnnotationSceneContent.objects: [0]: AnnotationObject.name: "
  "invalid fixed text size: AnnotationText.size=0, expected 1..=96";
 f.write("input.jsonl", J::array(
                         {{{"event", "browser.server.peer_opened"}, {"value", 1}, {"steady_ns", 100}}, {{"event", "iced.transport.disconnected"}, {"message", message}, {"steady_ns", 200}},
                          {{"event", "browser.server.peer_closed"}, {"value", 1}, {"steady_ns", 300}}}));
 auto result = f.query({"input.jsonl", "--error", "invalid server CBOR record: invalid fixed text size", "--triage"});
 EQ(result.status, 0);
 includes(result.out, {"all-words fallback", "AnnotationObject.name", "AnnotationText.size=0", "browser.server.peer_closed"});
}
void test_triage_pasted_error_retains_earliest_physical_and_timestamp_neighbors() {
 FileFixture f;
 auto pasted = pasted_error_fixture(f);
 auto result = f.query({"--error", pasted, "--triage"});
 EQ(result.status, 0);
 HAS(result.out, "exact/display-normalized phrase");
 includes(first_match_line(result.out), {"57-2000060000.log:4", "19479.000ms"});
 includes(result.out, {"Gallery(Measured", "Detail(CloseRequested)", "Dataset(OpenRequested)", "browser.intent.accepted", "browser.reply.sent", "SIGTERM", "file-mtime proximity"});
 HAS(result.err, "No non-empty strong identity");
 LACKS(result.out, "57-3000060000.log");
}
void test_triage_pasted_error_context_survives_noisy_historical_identity_chain() {
 FileFixture f;
 auto pasted = pasted_error_fixture(f);
 auto path = f.root / "build/validation/presentation-firefox.log.history/57-2000060000.log";
 auto original = support::read_text(path);
 std::string old;
 for (int i = 0; i < 100; ++i) {
  std::ostringstream surface;
  surface << std::hex << std::setfill('0') << std::setw(32) << i + 1;
  old += fixture_json(J{{"event", "render.failed"}, {"dataset_identity", 7}, {"surface", surface.str()}, {"elapsed_ms", 500 + i}}) + '\n';
 }
 support::write_text(path, old + original + "{\"event\":\"render.presented\",\"dataset_identity\":7,\"elapsed_ms\":19480}\n");
 set_mtime(path, INT64_C(1700000019500000000));
 auto result = f.query({"--error", pasted, "--triage", "--limit", "12"});
 EQ(result.status, 0);
 includes(result.out, {"Gallery(Measured", "Detail(CloseRequested)", "Dataset(OpenRequested)", "integration.failed", "anchor-failure"});
}
void test_triage_recognizes_generic_terminal_suffix_without_guessing_numeric_outcome() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"new.subsystem.terminal","sequence":2,"outcome":4}])json"));
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 includes(result.out, {"terminal evidence (not necessarily failure)", "outcome=4", "not a process/test exit"});
 LACKS(result.out, "anchor-failure");
}
void test_triage_terminal_summary_survives_failure_volume_and_preserves_unknown_fields() {
 FileFixture f;
 J rows = j(R"json([{"event":"new.stage.terminal","sequence":2,"packed_bits":4096,"outcome":0}])json");
 for (int i = 0; i < 30; ++i) rows.push_back({{"event", "failure." + std::to_string(i) + ".failed"}, {"request_id", i + 1}});
 f.write("input.jsonl", rows);
 auto result = f.query({"input.jsonl", "--triage", "--limit", "1"});
 EQ(result.status, 0);
 includes(result.out, {"new.stage.terminal", "packed_bits=4096", "stage terminals need not mean run completion"});
 f.write("status.jsonl", j(R"json([{"event":"opaque","terminal":true,"status":"failed"}])json"));
 HAS(f.query({"status.jsonl", "--triage"}).out, "anchor-failure");
}
void test_triage_untrusted_field_names_cannot_escape_size_or_terminal_output_bounds() {
 FileFixture f;
 J hostile = {{"event", "operation.terminal"}, {"evil\x1b[31m_id", "data"}, {std::string(10000, 'x'), "payload"}};
 f.write("input.jsonl", J::array({hostile}));
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 LACKS(result.out, "\x1b");
 FALSE(logs::strong_identities(record(hostile)));
 auto exported = f.exported({"input.jsonl", "--triage"});
 CHECK(exported.rows[0]["_log"]["triage_payload_truncated"]);
 for (auto it = exported.rows[0]["data"].begin(); it != exported.rows[0]["data"].end(); ++it) CHECK(logs::chars(it.key()) <= logs::MAX_TRIAGE_TEXT + 1);
}
void test_triage_handles_unmatched_start_duplicate_end_and_failed_span() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"work.started","span_id":1,"steady_ns":10},{"event":"work.completed","span_id":1,"steady_ns":20,"span_outcome":"failed"},{"event":"work.completed","span_id":1,"steady_ns":30},{"event":"read.started","span_id":1,"steady_ns":40}])json"));
 auto result = f.query({"input.jsonl", "--triage", "--top", "20"});
 EQ(result.status, 0);
 includes(result.out, {"failed-span", "duplicate-terminal", "missing-counterpart", "read: 1 start(s)"});
 LACKS(result.out, "work: 1 start(s)");
}
void test_triage_request_submit_start_progress_is_not_three_unfinished_operations() {
 FileFixture f;
 f.write(
  "input.jsonl", j(R"json([{"event":"job.requested","request_id":1},{"event":"job.submitted","request_id":1},{"event":"job.started","request_id":1},{"event":"job.completed","request_id":1}])json"));
 auto result = f.query({"input.jsonl", "--triage", "-q", "request_id=1"});
 EQ(result.status, 0);
 excludes(result.out, {"missing-counterpart", "duplicate-terminal"});
 result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 1);
 LACKS(result.out, "unclosed start");
}
void test_triage_secondary_handle_and_identity_handoff_do_not_fake_missing_completion() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"claim.requested","surface_high":1,"surface_low":2,"texture_id":"TextureId(3,1)"},{"event":"claim.outcome","surface_high":1,"surface_low":2},{"event":"job.requested","request_id":1,"span_id":2},{"event":"job.completed","request_id":1},{"event":"probe.failed","surface_high":1,"surface_low":2,"request_id":1}])json"));
 auto result = f.query({"input.jsonl", "--triage", "--top", "20"});
 EQ(result.status, 0);
 LACKS(result.out, "missing-counterpart");
 includes(result.out, {"handoff/instrumentation mismatch possible", "source-order input.jsonl:"});
}
void test_triage_unrelated_completion_is_not_an_identity_handoff() {
 FileFixture f;
 for (auto& rows :
  j(
   R"json([[{"event":"job.started","request_id":1},{"event":"job.completed","request_id":2}],[{"event":"job.started","span_id":1,"trace_id":7},{"event":"job.completed","span_id":2,"trace_id":7}],[{"event":"job.started","request_id":1},{"event":"job.started","request_id":1},{"event":"job.completed","request_id":1}]])json")) {
  f.write("input.jsonl", rows);
  auto result = f.query({"input.jsonl", "--triage", "-q", "*", "--top", "20"});
  EQ(result.status, 0);
  includes(result.out, {"missing-counterpart", "job: 1 start(s)"});
  LACKS(result.out, "identity-handoff");
 }
}
void test_triage_prefers_explicit_divergence_over_earlier_unclosed_start() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"background.started","request_id":1},{"event":"probe.missing","request_id":1},{"event":"probe.failed","request_id":1}])json"));
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 auto lines = support::lines(between(result.out, "Earliest divergence candidates", "\xff"));
 CHECK(lines.size() >= 2);
 HAS(lines[1], "input.jsonl:2: incomplete-state");
}
void test_triage_retains_native_spans_and_accepted_requests_after_busy_browser_capture() {
 FileFixture f;
 J browser = J::array();
 for (std::size_t i = 0; i < logs::MAX_TRIAGE_STATES + 1; ++i) browser.push_back({{"event", "sample.started"}, {"request_id", i + 1}});
 f.write("capture-firefox.log", browser);
 auto product = j(R"json({"source_session":5,"source_instance":1,"source_revision":4})json");
 J rows = J::array(
  {j(R"json({"event":"upscale.warm.runtime.started","span_id":580,"span_outcome":0,"steady_ns":1000000000})json"),
   j(R"json({"event":"browser.intent.accepted","participant":"Presentation.Select","sequence":1258,"value":17,"steady_ns":1008000000})json"),
   updated(product,
    j(
     R"json({"event":"presentation.workspace.wait","message":"producer_finalization_pending","requested_product_owner":19,"requested_product_revision":4,"admitted_allocation":37,"admitted_access":21,"admitted_generation":3,"admitted_display_held":false,"admitted_write_reserved":true,"admitted_completion_pending":true,"source_timeline_imported":true,"steady_ns":1038000000})json"))});
 f.write("capture.jsonl", rows);
 Strings args = {"capture-firefox.log", "capture.jsonl", "--triage", "--limit", "80"};
 auto result = f.query(args);
 EQ(result.status, 0);
 includes(result.out, {"unclosed instrumented span", "upscale.warm.runtime.started", "workspace wait without a captured frame edge: producer_finalization_pending", "participant=Presentation.Select",
                       "sequence=1258 value=17", "admitted_allocation=37", "admitted_access=21 admitted_generation=3 admitted_display_held=false",
                       "admitted_write_reserved=true admitted_completion_pending=true", "source_timeline_imported=true"});
 rows.push_back(j(R"json({"event":"upscale.warm.runtime.completed","span_id":580,"span_outcome":2,"steady_ns":1050000000})json"));
 rows.push_back(updated(product, {{"event", "presentation.frame.edge"}, {"steady_ns", 1060000000}}));
 f.write("capture.jsonl", rows);
 result = f.query(args);
 EQ(result.status, 0);
 excludes(result.out, {"unclosed instrumented span", "workspace wait without a captured frame edge:"});
}
std::pair<J, J> pixel_bridge_records() {
 auto common = j(R"json({"surface":"00000000000000010000000000000002","presentation_revision":7,"transfer_sequence":9})json");
 return {
  updated(common, {{"event", "firefox.workspace.pixel"}, {"boundary", "import"}, {"sample_index", 0}, {"sample_x", 4}, {"sample_y", 5}, {"sample_rgba", 10}}),
  updated(common, {{"event", "firefox.workspace.frame_forwarded"}, {"source", "00000000000000030000000000000004"}})
 };
}
void test_triage_missing_pixel_bridge_is_a_visible_anchor_and_finding() {
 FileFixture f;
 auto [pixel, bridge] = pixel_bridge_records();
 f.write("input.jsonl", J::array({pixel}));
 for (auto extra : std::vector<Strings>{{}, {"-q", "@event=firefox.workspace.pixel"}}) {
  auto result = f.query(plus({"input.jsonl", "--triage"}, extra));
  EQ(result.status, 0);
  includes(result.out, {"pixel-transfer-missing", "presentation_revision=7 transfer_sequence=9", "Highlights:"});
 }
 auto result = f.exported({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 auto& pending = only(result.rows);
 EQ(pending["data"], pixel);
 HAS(pending["_log"]["match"], "pixel-transfer-missing");
 FALSE(pending["data"].contains("source"));
}
void test_triage_explicit_pixel_query_does_not_exclude_its_later_bridge() {
 FileFixture f;
 auto [pixel, bridge] = pixel_bridge_records();
 for (auto& rows : {J::array({pixel, bridge}), J::array({bridge, pixel})}) {
  f.write("input.jsonl", rows);
  auto result = f.query({"input.jsonl", "--triage", "-q", "@event=firefox.workspace.pixel"});
  EQ(result.status, 0);
  excludes(result.out, {"pixel-transfer-missing", "pixel-transfer-conflict"});
  result = f.query({"input.jsonl", "--triage"});
  EQ(result.status, 1);
  LACKS(result.out, "pixel-transfer-missing");
 }
}
void test_triage_pixel_query_keeps_conflicting_bridge_evidence() {
 FileFixture f;
 auto [pixel, bridge] = pixel_bridge_records();
 auto conflict = updated(bridge, {{"source", "00000000000000050000000000000006"}});
 f.write("input.jsonl", J::array({pixel, bridge, conflict}));
 auto result = f.query({"input.jsonl", "--triage", "-q", "@event=firefox.workspace.pixel"});
 EQ(result.status, 0);
 HAS(result.out, "pixel-transfer-conflict");
 LACKS(result.out, "pixel-transfer-missing");
}
void test_triage_highlights_first_deterministic_pixel_chain_divergence() {
 FileFixture f;
 auto common = j(R"json({"presentation_revision":7,"transfer_sequence":3,"source":"00000000000000030000000000000004","sample_index":4,"sample_x":8,"sample_y":9})json");
 const std::string surface = "00000000000000010000000000000002";
 for (int rgba : {11, 10}) {
  J rows = J::array(
   {updated(common, {{"event", "firefox.workspace.frame_forwarded"}, {"surface", surface}}),
    updated(common, {{"event", "presentation.pixel"}, {"surface_high", 1}, {"surface_low", 2}, {"sample_rgba", 10}}),
    updated(common, {{"event", "firefox.workspace.pixel"}, {"surface", surface}, {"boundary", "import"}, {"sample_rgba", rgba}}),
    updated(common, {{"event", "firefox.workspace.pixel"}, {"surface", surface}, {"boundary", "mailbox"}, {"sample_rgba", rgba}}),
    updated(common, {{"event", "iced.surface.pixel"}, {"surface", surface}, {"sample_rgba", rgba}})});
  f.write("input.jsonl", rows);
  auto result = f.query(rgba == 11 ? Strings{"input.jsonl", "--triage", "--top", "20"} : Strings{"input.jsonl", "--triage"});
  EQ(result.status, rgba == 11 ? 0 : 1);
  if (rgba == 11)
   includes(result.out, {"pixel-chain-divergence", "native -> import sample differs", "presentation_revision=7 sample_index=4"});
  else
   LACKS(result.out, "pixel-chain-divergence");
 }
}
void test_tap_outcomes_keep_failed_test_and_diagnostic_context() {
 FileFixture f;
 f.write("node.log",
  "TAP version 13\n# Subtest: expected failure stays quiet\nok 1 - expected failure stays quiet\n# Subtest: scoped retirement\nnot ok 2 - scoped retirement\n  ---\n  error: |-\n      {\n    +   "
  "clock: 16,\n    -   clock: 14,\n      }\n  ...\nok 3 - optional failure fixture # SKIP unavailable\n# tests 3\n# pass 1\n# fail 1\n# skipped 1");
 auto result = f.exported({"node.log", "--errors", "--no-auto-correlate"});
 EQ(result.status, 0);
 EQ(column(result.rows, "data", "event"), (std::vector<J>{"tap.test_failed", "tap.summary"}));
 EQ(result.rows[0]["_log"]["test"], J("scoped retirement"));
 EQ(result.rows[1]["data"]["count"], J(1));
 auto context = f.exported({"node.log", "-q", "@test=\"scoped retirement\"", "--no-auto-correlate"});
 CHECK(any(context.rows, [](auto& r) { return contains(r["text"], "clock: 16"); }));
 CHECK(all(context.rows, [](auto& r) { return !truth(r["_log"]["parse_error"]); }));
 for (auto message : {"ok 1 - expected failure", "# fail 0", "not ok 3 - pending # TODO not implemented"}) FALSE(logs::parse_record("node.log", 1, message).get("@error"));
 CHECK(logs::parse_record("node.log", 1, "# tests 0").get("@error"));
}
void test_libtest_failures_keep_named_panic_output() {
 FileFixture f;
 f.write("rust.log",
  "test view::failed_input ... ok\ntest view::retirement ... FAILED\ntest view::layout ... ok\n---- view::retirement stdout ----\nthread 'view::retirement' (123) panicked at "
  "src/view.rs:20:5:\nassertion failed: retained_owner\n  left: 0\n right: 12\ntest result: FAILED. 2 passed; 1 failed; 0 ignored; 0 measured; 0 filtered out");
 auto result = f.exported({"rust.log", "--errors", "--no-auto-correlate"});
 EQ(result.status, 0);
 auto failed = select(result.rows, [](auto& r) { return r["data"].value("event", "") == "libtest.test_failed"; });
 EQ(only(failed)["_log"]["test"], J("view::retirement"));
 EQ(result.rows.back()["data"]["event"], J("libtest.summary"));
 auto context = f.exported({"rust.log", "-q", "@test=\"view::retirement\"", "--no-auto-correlate"});
 for (auto text : {"src/view.rs:20:5", "left: 0"}) CHECK(any(context.rows, [&](auto& r) { return contains(r["text"], text); }));
 for (auto message : {"test view::failure ... ok", "test view::failure ... ignored, no device", "test result: ok. 3 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out"})
  FALSE(logs::parse_record("rust.log", 1, message).get("@error"));
}
void test_training_failure_is_an_explicit_triage_anchor() {
 FileFixture f;
 f.write("metrics.jsonl",
  j(
   R"json([{"format_version":3,"role":"Live","run_id":"run-a","attempt_id":"attempt-b","progress":{"phase":"Train","class_error":99.0}},{"format_version":3,"role":"Terminal","run_id":"run-a","attempt_id":"attempt-b","progress":{"phase":"Error"}}])json"));
 auto exported = f.exported({"metrics.jsonl", "--errors"});
 EQ(exported.status, 0);
 EQ(only(exported.rows)["data"]["attempt_id"], J("attempt-b"));
 EQ(exported.rows[0]["_log"]["line"], J(2));
 auto result = f.query({"metrics.jsonl", "--triage"});
 EQ(result.status, 0);
 includes(result.out, {"anchor-failure", "failed outcome/span", "run-a", "attempt-b", "training.live.train", "training.terminal.error", "owners: training"});
 LACKS(result.out, "<object object");
}
void test_training_observations_do_not_invent_generic_begin_end_pairs() {
 FileFixture f;
 auto identity = j(R"json({"format_version":3,"run_id":"run-a","attempt_id":"attempt-b"})json");
 J configuration = J::object();
 for (int i = 0; i < 200; ++i) configuration[std::to_string(i)] = i;
 f.write("metrics.jsonl", J::array(
                           {updated(identity, {{"role", "Boundary"}, {"sequence", 1}, {"attempt_configuration", configuration}, {"progress", {{"phase", "Starting"}}}}),
                            updated(identity, j(R"json({"role":"Boundary","sequence":2,"progress":{"phase":"Train"}})json")),
                            updated(identity, j(R"json({"role":"Terminal","sequence":3,"progress":{"phase":"Error"}})json"))}));
 auto result = f.query({"metrics.jsonl", "--triage"});
 EQ(result.status, 0);
 includes(result.out, {"anchor-failure", "failed outcome/span", "1:training.boundary.starting", "training.boundary.train", "training.terminal.error", "run-a", "attempt-b"});
 excludes(result.out, {"unmatched-end", "missing-counterpart"});
 auto rows = f.exported({"metrics.jsonl", "--triage"}).rows;
 auto first = select(rows, [](auto& r) { return r["_log"]["line"] == 1; });
 CHECK(first);
 EQ(first[0]["data"]["progress"]["phase"], J("Starting"));
 EQ(first[0]["data"]["sequence"], J(1));
 EQ(first[0]["data"]["run_id"], J("run-a"));
 f.write("ordinary.jsonl", j(R"json([{"event":"job.failed","request_id":"ordinary-a"}])json"));
 result = f.query({"ordinary.jsonl", "--triage"});
 EQ(result.status, 0);
 HAS(result.out, "unmatched-end");
}
void test_triage_missing_events_have_stable_display() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"request_id":"request-a","detail":"plain observation"},{"request_id":"request-a","event":"job.failed"}])json"));
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 HAS(result.out, "(not recorded)");
 LACKS(result.out, "<object object");
}
void test_triage_groups_repeated_anomalies_without_losing_counts_or_context() {
 FileFixture f;
 J rows = J::array();
 for (int i = 0; i < 100; ++i) rows.push_back({{"event", "draw.missing"}, {"request_id", 1}});
 rows.push_back({{"event", "probe.failed"}, {"request_id", 1}});
 f.write("input.jsonl", rows);
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 auto highlights = between(result.out, "Highlights:", "Earliest divergence");
 EQ(occurrences(highlights, "incomplete-state"), 1U);
 HAS(highlights, "100 observations");
}
void test_triage_first_catch_info_context_recognizes_singular_plural_and_inline_json() {
 FileFixture f;
 for (const std::string count : {"1 message", "2 messages"}) {
  f.write("context.log",
   "[ RUN ] copying_test\n/workspace/test.cpp:4: failed: false with " + count + ": 'native diagnostics: {\"event\":\"job.completed\",\"span_id\":7}\n{\"event\":\"job.completed\",\"span_id\":7}\n");
  auto result = f.query({"context.log", "--triage"});
  EQ(result.status, 0);
  HAS(result.out, "copies=2");
  LACKS(result.out, "duplicate-terminal");
 }
}
void test_triage_zero_hops_still_includes_bounded_counter_refinement() {
 FileFixture f;
 J rows = j(R"json([{"event":"probe.failed","request_id":1,"generation":4,"slot":0,"steady_ns":10}])json");
 for (int i = 0; i < 5; ++i) rows.push_back({{"event", "noise"}});
 for (auto& r :
  j(
   R"json([{"event":"counter.evidence","generation":4,"slot":0,"steady_ns":20},{"event":"wrong.generation","generation":5,"slot":0,"steady_ns":21},{"event":"wrong.type","generation":"4","slot":0,"steady_ns":22},{"event":"bare.sequence","sequence":4,"steady_ns":23}])json"))
  rows.push_back(r);
 f.write("input.jsonl", rows);
 auto result = f.exported({"input.jsonl", "--triage", "--triage-hops", "0", "--near-ms", "0"});
 auto counter = select(result.rows, [](auto& r) { return r["data"].value("event", "") == "counter.evidence"; });
 CHECK(counter);
 HAS(counter[0]["_log"]["match"], "counter refinement");
 for (auto event : {"wrong.generation", "wrong.type", "bare.sequence"}) FALSE(events(result.rows).contains(event));
}
void test_triage_incomplete_end_is_an_anchor_without_claiming_failure() {
 FileFixture f;
 f.write("input.jsonl", j(R"json([{"event":"upload.started","request_id":7}])json"));
 auto result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 0);
 includes(result.out, {"unclosed start at capture end", "missing-counterpart", "not a proven product requirement"});
 HAS(result.err, "partial captures");
 f.write("input.jsonl", j(R"json([{"event":"ordinary.event"}])json"));
 result = f.query({"input.jsonl", "--triage"});
 EQ(result.status, 1);
 HAS(result.out, "no deterministic lifecycle anomaly");
}
void test_triage_gaps_use_only_same_source_actual_clock_and_detect_regression() {
 FileFixture f;
 f.write("input.jsonl",
  j(
   R"json([{"event":"one","request_id":4,"steady_ns":1},{"event":"two","request_id":4,"steady_ns":2000000001},{"event":"three","request_id":4,"elapsed_ms":10000},{"event":"four","request_id":4,"elapsed_ms":9999},{"event":"job.failed","request_id":4}])json"));
 auto result = f.query({"input.jsonl", "--triage", "--top", "20"});
 EQ(result.status, 0);
 includes(result.out, {"2000.000ms", "clock-regression"});
 LACKS(result.out, "8000.000ms");
 LACKS(f.query({"input.jsonl", "--triage", "--triage-gap-ms", "3000"}).out, "gap:");
 f.write("input.jsonl", j(R"json([{"event":"before","request_id":4,"elapsed_ms":0},{"event":"job.failed","request_id":4,"steady_ns":100000000000}])json"));
 LACKS(f.query({"input.jsonl", "--triage"}).out, "gap [");
}
void test_triage_hops_are_capped_and_empty_ids_do_not_expand() {
 FileFixture f;
 J rows = j(R"json([{"event":"seed.failed","request_id":1,"trace_id":0}])json");
 for (int i = 0; i < 10; ++i) rows.push_back({{"event", "padding"}});
 rows.push_back(j(R"json({"event":"hop.one","request_id":1,"span_id":2})json"));
 for (int i = 0; i < 10; ++i) rows.push_back({{"event", "padding"}});
 for (auto& r : j(R"json([{"event":"hop.two","span_id":2,"trace_id":3},{"event":"hop.three","trace_id":3,"request_id":4},{"event":"zero","trace_id":0}])json")) rows.push_back(r);
 f.write("input.jsonl", rows);
 auto zero = events(f.exported({"input.jsonl", "--triage", "-q", "event=seed.failed", "--triage-hops", "0", "--format", "jsonl"}).rows);
 CHECK(zero.contains("hop.one"));
 FALSE(zero.contains("hop.two"));
 auto one = events(f.exported({"input.jsonl", "--triage", "-q", "event=seed.failed", "--triage-hops", "1"}).rows);
 CHECK(one.contains("hop.two"));
 FALSE(one.contains("hop.three"));
 FALSE(one.contains("zero"));
}
void test_triage_bounds_cardinality_payload_and_default_output_without_related_run() {
 FileFixture f;
 J rows = J::array({{{"event", "seed.failed"}, {"request_id", 1}, {"detail", std::string(10000, 'x')}}});
 for (int i = 0; i < 1000; ++i) rows.push_back({{"event", "stage." + std::to_string(i)}, {"request_id", 1}, {"span_id", i + 10}});
 for (int i = 0; i < 1000; ++i) rows.push_back({{"event", "unrelated.noise"}, {"request_id", 9000}});
 f.write("input.jsonl", rows);
 Strings args = {"input.jsonl", "--triage", "--triage-identities", "3", "--limit", "7"};
 auto result = f.exported(args);
 EQ(result.status, 0);
 EQ(result.rows.size(), 7U);
 CHECK(any(result.rows, [](auto& r) { return truth(r["_log"].value("triage_payload_truncated", J(nullptr))); }));
 CHECK(all(result.rows, [](auto& r) { return logs::chars(r["text"].template get<std::string>()) <= logs::MAX_TRIAGE_TEXT + 1; }));
 FALSE(events(result.rows).contains("unrelated.noise"));
 includes(result.err, {"Identity budget reached", "capacity reached"});
 EQ(f.exported(args).rows, result.rows);
 for (auto option : {"--related-run", "--tail", "--list-runs"}) EQ(f.query({"input.jsonl", "--triage", option}).status, 2);
}
void test_triage_run_reuse_where_and_context_copies_are_not_false_duplicates() {
 FileFixture f;
 family_fixture(f);
 auto result = f.exported({"--family", "session", "--history", "--run", "current", "--triage", "-q", "buffer=\"BufferId(21,1)\""});
 EQ(column_set(result.rows, "_log", "run"), std::set<J>{"build/validation/session@current"});
 f.write("copy.log",
  "[ RUN ] copied_test\n/workspace/test.cpp:4: failed: false with 1 message: 'native diagnostics:\n{\"event\":\"job.completed\",\"span_id\":7}\n{\"event\":\"job.completed\",\"span_id\":7}\n");
 auto report = f.query({"copy.log", "--triage"});
 EQ(report.status, 0);
 LACKS(report.out, "duplicate-terminal");
 HAS(report.out, "copies=2");
 f.write("input.jsonl", j(R"json([{"event":"work.started","request_id":1},{"event":"work.failed","request_id":1},{"event":"excluded","request_id":1}])json"));
 FALSE(events(f.exported({"input.jsonl", "--triage", "--where", "NOT event=excluded"}).rows).contains("excluded"));
}
void test_triage_parse_damage_is_bounded_evidence_and_strict_remains_strict() {
 FileFixture f;
 f.write("broken.jsonl", "{\"event\":\n{\"event\":\"worker.completed\",\"span_outcome\":2,\"span_id\":4}\n");
 auto result = f.query({"broken.jsonl", "--triage"});
 EQ(result.status, 0);
 HAS(result.out, "parse failure");
 HAS(result.err, "parse damage is not itself a product failure");
 LACKS(result.out, "failed-span");
 EQ(f.query({"broken.jsonl", "--triage", "--strict"}).status, 2);
 EQ(f.query({"broken.jsonl", "--triage", "--strict", "--where", "span_id=4"}).status, 2);
}
constexpr Case cases[]{
 {"NativeToolTests.test_bootstrap_atomic_publication_concurrency_and_spaces", test_bootstrap_atomic_publication_concurrency_and_spaces},
 {"NativeToolTests.test_tools_tidy_grouped_selection_and_module_resume", test_tools_tidy_grouped_selection_and_module_resume},
 {"NativeToolTests.test_tools_tidy_waits_for_bootstrap_database_publication", test_tools_tidy_waits_for_bootstrap_database_publication},
 {"NativeToolTests.test_bootstrap_waits_for_tidy_recomputes_inputs_and_shares_cache_subroots", test_bootstrap_waits_for_tidy_recomputes_inputs_and_shares_cache_subroots},
 {"NativeToolTests.test_tools_lock_wait_cancellation_and_failure_release", test_tools_lock_wait_cancellation_and_failure_release},
 {"NativeToolTests.test_tools_cold_root_ownership_handoff", test_tools_cold_root_ownership_handoff, true},
 {"NativeToolTests.test_tools_ownership_selection_keeps_normal_fixture_restrictions", test_tools_ownership_selection_keeps_normal_fixture_restrictions},
 {"NativeToolTests.test_native_package_inventory_completeness_and_stable_archive_identity", test_native_package_inventory_completeness_and_stable_archive_identity},
 {"NativeToolTests.test_package_callers_preserve_status_output_and_publication", test_package_callers_preserve_status_output_and_publication},
 {"NativeToolTests.test_package_callers_cancel_and_reap_archive_and_hash_descendants", test_package_callers_cancel_and_reap_archive_and_hash_descendants},
 {"NativeToolTests.test_native_process_selection_artifact_bounds_and_actions", test_native_process_selection_artifact_bounds_and_actions},
 {"NativeToolTests.test_tools_tidy_and_oracle_mount_diagnostic_runtime_readonly", test_tools_tidy_and_oracle_mount_diagnostic_runtime_readonly},
 {"NativeToolTests.test_native_compiler_launcher_preserves_arguments_cache_and_status", test_native_compiler_launcher_preserves_arguments_cache_and_status},
 {"ParserTests.test_boolean_precedence_implicit_and_and_quoted_terms", test_boolean_precedence_implicit_and_and_quoted_terms},
 {"ParserTests.test_exact_numeric_values_preserve_uint64_and_boolean_type", test_exact_numeric_values_preserve_uint64_and_boolean_type},
 {"ParserTests.test_presence_null_and_missing_comparisons", test_presence_null_and_missing_comparisons},
 {"ParserTests.test_substrings_regex_and_nested_fields", test_substrings_regex_and_nested_fields},
 {"ParserTests.test_invalid_queries_report_errors", test_invalid_queries_report_errors},
 {"LogFormatTests.test_intent_request_projection_keeps_exact_uint64_and_rejects_other_counters", test_intent_request_projection_keeps_exact_uint64_and_rejects_other_counters},
 {"LogFormatTests.test_training_terminal_errors_keep_run_and_attempt", test_training_terminal_errors_keep_run_and_attempt},
 {"LogFormatTests.test_training_session_model_and_first_cause_survive_bounded_triage", test_training_session_model_and_first_cause_survive_bounded_triage},
 {"LogFormatTests.test_training_record_near_misses_do_not_project_lifecycle", test_training_record_near_misses_do_not_project_lifecycle},
 {"LogFormatTests.test_training_snapshot_reserves_canonical_facts_before_optional_payloads", test_training_snapshot_reserves_canonical_facts_before_optional_payloads},
 {"LogFormatTests.test_complete_jsonl_decodes_each_payload_once", test_complete_jsonl_decodes_each_payload_once},
 {"LogFormatTests.test_runtime_and_wrapped_json_keep_original_vocabulary", test_runtime_and_wrapped_json_keep_original_vocabulary},
 {"LogFormatTests.test_surface_identity_has_one_exact_projection", test_surface_identity_has_one_exact_projection},
 {"LogFormatTests.test_workspace_source_is_separate_from_artifact_and_arena", test_workspace_source_is_separate_from_artifact_and_arena},
 {"LogFormatTests.test_transfer_correlation_uses_source_and_rejects_arena_counter_aliases", test_transfer_correlation_uses_source_and_rejects_arena_counter_aliases},
 {"LogFormatTests.test_memory_provenance_process_ids_never_expand_as_resource_identities", test_memory_provenance_process_ids_never_expand_as_resource_identities},
 {"LogFormatTests.test_native_header_and_key_value_messages", test_native_header_and_key_value_messages},
 {"LogFormatTests.test_native_fatal_components_and_detail_keep_the_reporter_bounds", test_native_fatal_components_and_detail_keep_the_reporter_bounds},
 {"LogFormatTests.test_firefox_fatal_envelopes_preserve_context_and_opaque_status", test_firefox_fatal_envelopes_preserve_context_and_opaque_status},
 {"LogFormatTests.test_process_projections_require_exact_consistent_observed_ids", test_process_projections_require_exact_consistent_observed_ids},
 {"LogFormatTests.test_vulkan_report_conditions_do_not_become_allocation_or_fd_identity_facts", test_vulkan_report_conditions_do_not_become_allocation_or_fd_identity_facts},
 {"LogFormatTests.test_vulkan_object_projection_keeps_types_and_multiple_images_separate", test_vulkan_object_projection_keeps_types_and_multiple_images_separate},
 {"LogFormatTests.test_firefox_moz_log_including_optional_timestamp", test_firefox_moz_log_including_optional_timestamp},
 {"LogFormatTests.test_embedded_json_and_unstructured_records_keep_provenance", test_embedded_json_and_unstructured_records_keep_provenance},
 {"LogFormatTests.test_json_and_plain_failure_candidates", test_json_and_plain_failure_candidates},
 {"LogFormatTests.test_malformed_json_remains_searchable", test_malformed_json_remains_searchable},
 {"LogFormatTests.test_compiler_progress_and_source_keep_failure_vocabulary_as_context", test_compiler_progress_and_source_keep_failure_vocabulary_as_context},
 {"LogFormatTests.test_clock_domains_and_nanosecond_precision", test_clock_domains_and_nanosecond_precision},
 {"LogFormatTests.test_non_scalar_event_and_overflowing_json_numbers_do_not_crash", test_non_scalar_event_and_overflowing_json_numbers_do_not_crash},
 {"LogFormatTests.test_signal_semantics_preserve_encoded_child_exit_status", test_signal_semantics_preserve_encoded_child_exit_status},
 {"LogFormatTests.test_transport_decode_and_allocator_failures_are_searchable_without_a_level", test_transport_decode_and_allocator_failures_are_searchable_without_a_level},
 {"LogFormatTests.test_catch_failure_preserves_parent_and_extracts_quoted_json", test_catch_failure_preserves_parent_and_extracts_quoted_json},
 {"LogFormatTests.test_wrapped_native_context_and_image_results", test_wrapped_native_context_and_image_results},
 {"LogFormatTests.test_catch_console_exception_keeps_location_and_placeholder_without_json_damage", test_catch_console_exception_keeps_location_and_placeholder_without_json_damage},
 {"LogFormatTests.test_catch_report_title_recovers_delayed_stderr_test_identity", test_catch_report_title_recovers_delayed_stderr_test_identity},
 {"TriageSelectionTests.test_reservoir_equal_ranks_preserve_insertion_priority_after_updates", test_reservoir_equal_ranks_preserve_insertion_priority_after_updates},
 {"TriageSelectionTests.test_saturated_reservoir_has_logarithmic_comparison_budget", test_saturated_reservoir_has_logarithmic_comparison_budget},
 {"TriageSelectionTests.test_counter_lookup_reads_each_field_once_at_identity_capacity", test_counter_lookup_reads_each_field_once_at_identity_capacity},
 {"TriageSelectionTests.test_counter_lookup_preserves_first_matching_schema_and_origin", test_counter_lookup_preserves_first_matching_schema_and_origin},
 {"TriageSelectionTests.test_automatic_anchor_search_is_logarithmic_and_returns_two_neighbors", test_automatic_anchor_search_is_logarithmic_and_returns_two_neighbors},
 {"TriageSelectionTests.test_pixel_edges_keep_the_same_evidence_for_every_arrival_order", test_pixel_edges_keep_the_same_evidence_for_every_arrival_order},
 {"TriageSelectionTests.test_direct_pixels_join_only_the_acquired_source_in_every_arrival_order", test_direct_pixels_join_only_the_acquired_source_in_every_arrival_order},
 {"TriageSelectionTests.test_direct_pixels_reject_copy_receipts_before_or_after_acquisition", test_direct_pixels_reject_copy_receipts_before_or_after_acquisition},
 {"TriageSelectionTests.test_optional_direct_image_edges_preserve_native_comparison_in_every_arrival_order", test_optional_direct_image_edges_preserve_native_comparison_in_every_arrival_order},
 {"TriageSelectionTests.test_optional_direct_image_requires_completion_marker_and_exact_direct_bridge", test_optional_direct_image_requires_completion_marker_and_exact_direct_bridge},
 {"TriageSelectionTests.test_complete_source_copy_pixels_compare_exact_edges_in_every_group_order", test_complete_source_copy_pixels_compare_exact_edges_in_every_group_order},
 {"TriageSelectionTests.test_optional_source_copy_absence_and_retired_alias_keep_required_native_evidence", test_optional_source_copy_absence_and_retired_alias_keep_required_native_evidence},
 {"TriageSelectionTests.test_optional_native_pixels_without_a_bridge_never_create_missing_required_evidence", test_optional_native_pixels_without_a_bridge_never_create_missing_required_evidence},
 {"TriageSelectionTests.test_source_copy_pixels_reject_conflicting_bridge_facts_in_either_order", test_source_copy_pixels_reject_conflicting_bridge_facts_in_either_order},
 {"TriageSelectionTests.test_source_copy_pixels_never_join_another_transfer_publication_surface_or_run", test_source_copy_pixels_never_join_another_transfer_publication_surface_or_run},
 {"TriageSelectionTests.test_source_copy_edges_reject_contradictory_coordinates_and_iced_source_facts", test_source_copy_edges_reject_contradictory_coordinates_and_iced_source_facts},
 {"TriageSelectionTests.test_source_copy_pixels_with_ambiguous_native_source_spelling_are_not_compared", test_source_copy_pixels_with_ambiguous_native_source_spelling_are_not_compared},
 {"TriageSelectionTests.test_source_copy_pixels_preserve_the_required_copy_mode_chain", test_source_copy_pixels_preserve_the_required_copy_mode_chain},
 {"TriageSelectionTests.test_source_copy_owner_mutation_keeps_the_first_complete_observation", test_source_copy_owner_mutation_keeps_the_first_complete_observation},
 {"TriageSelectionTests.test_native_pixel_timeline_retains_exact_source_copy_facts", test_native_pixel_timeline_retains_exact_source_copy_facts},
 {"TriageSelectionTests.test_bounded_failure_payload_preserves_reason_and_identities_before_large_context", test_bounded_failure_payload_preserves_reason_and_identities_before_large_context},
 {"TriageSelectionTests.test_absent_pixel_bridges_finalize_each_exact_transfer_once", test_absent_pixel_bridges_finalize_each_exact_transfer_once},
 {"TriageSelectionTests.test_exact_pixel_bridge_succeeds_before_or_after_the_pixel", test_exact_pixel_bridge_succeeds_before_or_after_the_pixel},
 {"TriageSelectionTests.test_pending_pixel_capacity_counts_replay_and_finalization_exactly", test_pending_pixel_capacity_counts_replay_and_finalization_exactly},
 {"TriageSelectionTests.test_saturated_bridge_index_reports_incomplete_for_later_pixels", test_saturated_bridge_index_reports_incomplete_for_later_pixels},
 {"TriageSelectionTests.test_saturated_bridge_index_resolves_pending_pixels_directly", test_saturated_bridge_index_resolves_pending_pixels_directly},
 {"TriageSelectionTests.test_saturated_bridge_direct_replay_preserves_corroborating_conflict", test_saturated_bridge_direct_replay_preserves_corroborating_conflict},
 {"TriageSelectionTests.test_full_but_untruncated_bridge_index_preserves_proven_missing", test_full_but_untruncated_bridge_index_preserves_proven_missing},
 {"TriageSelectionTests.test_missing_pixel_finalization_respects_finding_and_anchor_budgets", test_missing_pixel_finalization_respects_finding_and_anchor_budgets},
 {"TriageSelectionTests.test_source_less_pixel_requires_an_exact_forwarded_transfer", test_source_less_pixel_requires_an_exact_forwarded_transfer},
 {"TriageSelectionTests.test_forwarded_source_conflict_is_rejected", test_forwarded_source_conflict_is_rejected},
 {"TriageSelectionTests.test_pixel_owner_mutation_wins_before_cross_stage_comparison", test_pixel_owner_mutation_wins_before_cross_stage_comparison},
 {"TriageSelectionTests.test_pixel_transfer_comparisons_are_linear_in_samples", test_pixel_transfer_comparisons_are_linear_in_samples},
 {"FileQueryTests.test_json_documents_preserve_nested_facts_and_line_span", test_json_documents_preserve_nested_facts_and_line_span},
 {"FileQueryTests.test_damaged_json_documents_remain_searchable", test_damaged_json_documents_remain_searchable},
 {"FileQueryTests.test_statistics_preserve_integer_sums_buckets_and_full_match_counts", test_statistics_preserve_integer_sums_buckets_and_full_match_counts},
 {"FileQueryTests.test_statistics_separate_runs_sources_clocks_and_numeric_absence", test_statistics_separate_runs_sources_clocks_and_numeric_absence},
 {"FileQueryTests.test_statistics_report_capacity_and_truncated_inputs", test_statistics_report_capacity_and_truncated_inputs},
 {"FileQueryTests.test_explicit_intent_correlation_connects_browser_and_native_stages", test_explicit_intent_correlation_connects_browser_and_native_stages},
 {"FileQueryTests.test_vulkan_callback_preserves_complete_text_objects_and_physical_lines", test_vulkan_callback_preserves_complete_text_objects_and_physical_lines},
 {"FileQueryTests.test_vulkan_representatives_count_distinct_ids_without_shortening_messages", test_vulkan_representatives_count_distinct_ids_without_shortening_messages},
 {"FileQueryTests.test_vulkan_assembly_preserves_interruptions_and_reports_bounded_fragments", test_vulkan_assembly_preserves_interruptions_and_reports_bounded_fragments},
 {"FileQueryTests.test_vulkan_exact_handles_add_bounded_same_file_provenance_candidates", test_vulkan_exact_handles_add_bounded_same_file_provenance_candidates},
 {"FileQueryTests.test_vulkan_recorded_process_joins_files_in_one_run_and_rejects_other_processes", test_vulkan_recorded_process_joins_files_in_one_run_and_rejects_other_processes},
 {"FileQueryTests.test_forced_stop_joins_observed_child_records_without_reconstructing_historical_pids", test_forced_stop_joins_observed_child_records_without_reconstructing_historical_pids},
 {"FileQueryTests.test_fatal_process_links_distinguish_relationships_and_reject_conflicting_facts", test_fatal_process_links_distinguish_relationships_and_reject_conflicting_facts},
 {"FileQueryTests.test_fatal_envelopes_use_the_reader_byte_bound_and_keep_status_opaque", test_fatal_envelopes_use_the_reader_byte_bound_and_keep_status_opaque},
 {"FileQueryTests.test_vulkan_ambiguous_handle_owners_are_reported_without_asserted_provenance", test_vulkan_ambiguous_handle_owners_are_reported_without_asserted_provenance},
 {"FileQueryTests.test_automatic_timeline_interleaves_exact_frame_snapshot_and_publication", test_automatic_timeline_interleaves_exact_frame_snapshot_and_publication},
 {"FileQueryTests.test_upscale_settlement_joins_native_frontier_without_inventing_presentation_match", test_upscale_settlement_joins_native_frontier_without_inventing_presentation_match},
 {"FileQueryTests.test_timeline_failure_highlights_show_reason_original_event_and_provenance", test_timeline_failure_highlights_show_reason_original_event_and_provenance},
 {"FileQueryTests.test_automatic_machine_formats_require_opt_in_and_preserve_query_rows", test_automatic_machine_formats_require_opt_in_and_preserve_query_rows},
 {"FileQueryTests.test_automatic_recent_highlight_keeps_final_reopen_visible_after_phase_volume", test_automatic_recent_highlight_keeps_final_reopen_visible_after_phase_volume},
 {"FileQueryTests.test_automatic_scopes_reject_zero_generic_counter_conflicts_and_transitive_links", test_automatic_scopes_reject_zero_generic_counter_conflicts_and_transitive_links},
 {"FileQueryTests.test_automatic_ambiguous_projected_source_revisions_do_not_join", test_automatic_ambiguous_projected_source_revisions_do_not_join},
 {"FileQueryTests.test_automatic_control_proximity_is_labeled_and_bounded", test_automatic_control_proximity_is_labeled_and_bounded},
 {"FileQueryTests.test_automatic_bounds_diversity_global_capacity_and_existing_match_limit", test_automatic_bounds_diversity_global_capacity_and_existing_match_limit},
 {"FileQueryTests.test_automatic_respects_where_runs_and_explicit_correlation", test_automatic_respects_where_runs_and_explicit_correlation},
 {"FileQueryTests.test_automatic_payloads_and_scan_count_stay_bounded", test_automatic_payloads_and_scan_count_stay_bounded},
 {"FileQueryTests.test_discovery_deduplicates_paths_and_excludes_histories_by_default", test_discovery_deduplicates_paths_and_excludes_histories_by_default},
 {"FileQueryTests.test_discovery_captures_each_repeated_path_only_once", test_discovery_captures_each_repeated_path_only_once},
 {"FileQueryTests.test_directory_discovery_streams_entries_and_releases_handles", test_directory_discovery_streams_entries_and_releases_handles},
 {"FileQueryTests.test_empty_missing_and_outside_inputs_fail_clearly", test_empty_missing_and_outside_inputs_fail_clearly},
 {"FileQueryTests.test_file_boundaries_blank_lines_and_oversized_records", test_file_boundaries_blank_lines_and_oversized_records},
 {"FileQueryTests.test_captured_byte_size_and_correlation_stability", test_captured_byte_size_and_correlation_stability},
 {"FileQueryTests.test_recent_input_preserves_physical_lines_and_limits_evidence", test_recent_input_preserves_physical_lines_and_limits_evidence},
 {"FileQueryTests.test_retention_is_bounded_deterministic_and_counts_all_matches", test_retention_is_bounded_deterministic_and_counts_all_matches},
 {"FileQueryTests.test_clock_order_never_implies_cross_domain_alignment", test_clock_order_never_implies_cross_domain_alignment},
 {"FileQueryTests.test_correlation_is_direct_and_never_joins_empty_identities", test_correlation_is_direct_and_never_joins_empty_identities},
 {"FileQueryTests.test_composite_identity_keeps_sessions_separate", test_composite_identity_keeps_sessions_separate},
 {"FileQueryTests.test_memory_provenance_query_exposes_export_mapping_and_import_without_fd_identity_claim", test_memory_provenance_query_exposes_export_mapping_and_import_without_fd_identity_claim},
 {"FileQueryTests.test_descriptor_lineage_joins_exact_source_and_process_scoped_exports", test_descriptor_lineage_joins_exact_source_and_process_scoped_exports},
 {"FileQueryTests.test_descriptor_lineage_distinguishes_ofd_mismatch_unsupported_denied_and_unavailable", test_descriptor_lineage_distinguishes_ofd_mismatch_unsupported_denied_and_unavailable},
 {"FileQueryTests.test_descriptor_lineage_reports_fd_reuse_closed_fd_process_and_binding_conflicts", test_descriptor_lineage_reports_fd_reuse_closed_fd_process_and_binding_conflicts},
 {"FileQueryTests.test_descriptor_lineage_checks_native_export_argument_without_metadata_identity_inference",
  test_descriptor_lineage_checks_native_export_argument_without_metadata_identity_inference},
 {"FileQueryTests.test_descriptor_lineage_rejects_ambiguous_reused_export_reference", test_descriptor_lineage_rejects_ambiguous_reused_export_reference},
 {"FileQueryTests.test_descriptor_lineage_limits_and_where_remain_evidence_limits", test_descriptor_lineage_limits_and_where_remain_evidence_limits},
 {"FileQueryTests.test_vulkan_export_lineage_preserves_descriptor_roles_and_retry_results", test_vulkan_export_lineage_preserves_descriptor_roles_and_retry_results},
 {"FileQueryTests.test_vulkan_export_lineage_missing_success_and_conflicting_import_are_explicit", test_vulkan_export_lineage_missing_success_and_conflicting_import_are_explicit},
 {"FileQueryTests.test_vulkan_export_lineage_rejects_ambiguous_source_association", test_vulkan_export_lineage_rejects_ambiguous_source_association},
 {"FileQueryTests.test_vulkan_export_lineage_keeps_process_scoped_imports_and_null_success_visible", test_vulkan_export_lineage_keeps_process_scoped_imports_and_null_success_visible},
 {"FileQueryTests.test_vulkan_export_lineage_requires_retained_retirement_evidence", test_vulkan_export_lineage_requires_retained_retirement_evidence},
 {"FileQueryTests.test_vulkan_export_lineage_retirement_requires_released_handles_and_same_backing", test_vulkan_export_lineage_retirement_requires_released_handles_and_same_backing},
 {"FileQueryTests.test_vulkan_export_lineage_compares_every_immutable_retry_observation", test_vulkan_export_lineage_compares_every_immutable_retry_observation},
 {"FileQueryTests.test_vulkan_export_lineage_distinguishes_missing_source_association", test_vulkan_export_lineage_distinguishes_missing_source_association},
 {"FileQueryTests.test_vulkan_export_lineage_source_query_correlates_process_allocation", test_vulkan_export_lineage_source_query_correlates_process_allocation},
 {"FileQueryTests.test_loaded_library_mapping_inventory_preserves_actual_paths_and_limits", test_loaded_library_mapping_inventory_preserves_actual_paths_and_limits},
 {"FileQueryTests.test_surface_correlates_native_and_firefox_and_where_scopes_all_rows", test_surface_correlates_native_and_firefox_and_where_scopes_all_rows},
 {"FileQueryTests.test_context_is_bounded_deduplicated_and_obeys_where", test_context_is_bounded_deduplicated_and_obeys_where},
 {"FileQueryTests.test_dense_context_considers_buffered_rows_only_once", test_dense_context_considers_buffered_rows_only_once},
 {"FileQueryTests.test_projection_preserves_provenance_and_uint64", test_projection_preserves_provenance_and_uint64},
 {"FileQueryTests.test_error_filter_strict_mode_and_exit_statuses", test_error_filter_strict_mode_and_exit_statuses},
 {"FileQueryTests.test_summary_groups_are_deterministic_and_cardinality_is_bounded", test_summary_groups_are_deterministic_and_cardinality_is_bounded},
 {"FileQueryTests.test_help_documents_grammar_and_native_wrapper_is_executable", test_help_documents_grammar_and_native_wrapper_is_executable},
 {"FileQueryTests.test_wrapper_retains_both_streams_and_queryable_terminal_status", test_wrapper_retains_both_streams_and_queryable_terminal_status},
 {"FileQueryTests.test_fresh_all_stops_registered_tests_and_orphans_but_preserves_other_owners", test_fresh_all_stops_registered_tests_and_orphans_but_preserves_other_owners},
 {"FileQueryTests.test_gui_launch_replaces_runtime_sessions_and_preserves_other_owners", test_gui_launch_replaces_runtime_sessions_and_preserves_other_owners},
 {"FileQueryTests.test_gui_launch_replaces_an_owner_still_preparing_its_container", test_gui_launch_replaces_an_owner_still_preparing_its_container},
 {"FileQueryTests.test_delayed_older_gui_launch_preserves_the_newer_runtime", test_delayed_older_gui_launch_preserves_the_newer_runtime},
 {"FileQueryTests.test_family_and_history_pairing_retains_inference_and_archive_identity", test_family_and_history_pairing_retains_inference_and_archive_identity},
 {"FileQueryTests.test_family_discovers_separate_acceptance_and_rotating_mozilla_evidence", test_family_discovers_separate_acceptance_and_rotating_mozilla_evidence},
 {"FileQueryTests.test_family_inventory_groups_selectors_without_capturing_unrelated_entries", test_family_inventory_groups_selectors_without_capturing_unrelated_entries},
 {"FileQueryTests.test_family_symlink_reuses_earlier_target_candidates_and_checks_containment", test_family_symlink_reuses_earlier_target_candidates_and_checks_containment},
 {"FileQueryTests.test_child_mozilla_archived_only_family_and_direct_role", test_child_mozilla_archived_only_family_and_direct_role},
 {"FileQueryTests.test_mozilla_family_retains_containment_and_discovery_budget", test_mozilla_family_retains_containment_and_discovery_budget},
 {"FileQueryTests.test_history_catalog_enumerates_each_archive_directory_once", test_history_catalog_enumerates_each_archive_directory_once},
 {"FileQueryTests.test_shared_event_anchors_link_transcript_and_propagate_test_tags", test_shared_event_anchors_link_transcript_and_propagate_test_tags},
 {"FileQueryTests.test_related_run_includes_terminal_and_never_crosses_reused_buffer_ids", test_related_run_includes_terminal_and_never_crosses_reused_buffer_ids},
 {"FileQueryTests.test_transcript_incident_keeps_buffer_ids_failure_and_catch_context", test_transcript_incident_keeps_buffer_ids_failure_and_catch_context},
 {"FileQueryTests.test_pasted_multiline_error_finds_earliest_physical_record_and_nearby_family", test_pasted_multiline_error_finds_earliest_physical_record_and_nearby_family},
 {"FileQueryTests.test_pasted_error_uses_phrase_before_unordered_fallback", test_pasted_error_uses_phrase_before_unordered_fallback},
 {"FileQueryTests.test_pasted_error_selection_scans_once_with_exact_fallback_and_unicode", test_pasted_error_selection_scans_once_with_exact_fallback_and_unicode},
 {"FileQueryTests.test_error_neighborhood_respects_radius_and_machine_output_bounds", test_error_neighborhood_respects_radius_and_machine_output_bounds},
 {"FileQueryTests.test_error_lookup_reports_absence_and_rejects_empty_text", test_error_lookup_reports_absence_and_rejects_empty_text},
 {"FileQueryTests.test_triage_identity_namespaces_exclude_ambient_empty_and_bare_counters", test_triage_identity_namespaces_exclude_ambient_empty_and_bare_counters},
 {"FileQueryTests.test_triage_automatically_correlates_probe_surface_catch_native_and_counter_leaf", test_triage_automatically_correlates_probe_surface_catch_native_and_counter_leaf},
 {"FileQueryTests.test_triage_single_file_discovers_current_siblings_without_expanding_explicit_queries", test_triage_single_file_discovers_current_siblings_without_expanding_explicit_queries},
 {"FileQueryTests.test_triage_single_archive_discovers_only_its_rotation_batch_without_current_files", test_triage_single_archive_discovers_only_its_rotation_batch_without_current_files},
 {"FileQueryTests.test_triage_named_archives_match_exact_names_and_reject_external_history", test_triage_named_archives_match_exact_names_and_reject_external_history},
 {"FileQueryTests.test_triage_ranks_explicit_failure_above_error_related_events_without_changing_errors_filter",
  test_triage_ranks_explicit_failure_above_error_related_events_without_changing_errors_filter},
 {"FileQueryTests.test_triage_finds_short_reconnect_lifetimes_without_claiming_their_cause", test_triage_finds_short_reconnect_lifetimes_without_claiming_their_cause},
 {"FileQueryTests.test_pasted_transport_error_retains_field_cause_and_reconnect_context", test_pasted_transport_error_retains_field_cause_and_reconnect_context},
 {"FileQueryTests.test_triage_pasted_error_retains_earliest_physical_and_timestamp_neighbors", test_triage_pasted_error_retains_earliest_physical_and_timestamp_neighbors},
 {"FileQueryTests.test_triage_pasted_error_context_survives_noisy_historical_identity_chain", test_triage_pasted_error_context_survives_noisy_historical_identity_chain},
 {"FileQueryTests.test_triage_recognizes_generic_terminal_suffix_without_guessing_numeric_outcome", test_triage_recognizes_generic_terminal_suffix_without_guessing_numeric_outcome},
 {"FileQueryTests.test_triage_terminal_summary_survives_failure_volume_and_preserves_unknown_fields", test_triage_terminal_summary_survives_failure_volume_and_preserves_unknown_fields},
 {"FileQueryTests.test_triage_untrusted_field_names_cannot_escape_size_or_terminal_output_bounds", test_triage_untrusted_field_names_cannot_escape_size_or_terminal_output_bounds},
 {"FileQueryTests.test_triage_handles_unmatched_start_duplicate_end_and_failed_span", test_triage_handles_unmatched_start_duplicate_end_and_failed_span},
 {"FileQueryTests.test_triage_request_submit_start_progress_is_not_three_unfinished_operations", test_triage_request_submit_start_progress_is_not_three_unfinished_operations},
 {"FileQueryTests.test_triage_secondary_handle_and_identity_handoff_do_not_fake_missing_completion", test_triage_secondary_handle_and_identity_handoff_do_not_fake_missing_completion},
 {"FileQueryTests.test_triage_unrelated_completion_is_not_an_identity_handoff", test_triage_unrelated_completion_is_not_an_identity_handoff},
 {"FileQueryTests.test_triage_prefers_explicit_divergence_over_earlier_unclosed_start", test_triage_prefers_explicit_divergence_over_earlier_unclosed_start},
 {"FileQueryTests.test_triage_retains_native_spans_and_accepted_requests_after_busy_browser_capture", test_triage_retains_native_spans_and_accepted_requests_after_busy_browser_capture},
 {"FileQueryTests.test_triage_missing_pixel_bridge_is_a_visible_anchor_and_finding", test_triage_missing_pixel_bridge_is_a_visible_anchor_and_finding},
 {"FileQueryTests.test_triage_explicit_pixel_query_does_not_exclude_its_later_bridge", test_triage_explicit_pixel_query_does_not_exclude_its_later_bridge},
 {"FileQueryTests.test_triage_pixel_query_keeps_conflicting_bridge_evidence", test_triage_pixel_query_keeps_conflicting_bridge_evidence},
 {"FileQueryTests.test_triage_highlights_first_deterministic_pixel_chain_divergence", test_triage_highlights_first_deterministic_pixel_chain_divergence},
 {"FileQueryTests.test_tap_outcomes_keep_failed_test_and_diagnostic_context", test_tap_outcomes_keep_failed_test_and_diagnostic_context},
 {"FileQueryTests.test_libtest_failures_keep_named_panic_output", test_libtest_failures_keep_named_panic_output},
 {"FileQueryTests.test_training_failure_is_an_explicit_triage_anchor", test_training_failure_is_an_explicit_triage_anchor},
 {"FileQueryTests.test_training_observations_do_not_invent_generic_begin_end_pairs", test_training_observations_do_not_invent_generic_begin_end_pairs},
 {"FileQueryTests.test_triage_missing_events_have_stable_display", test_triage_missing_events_have_stable_display},
 {"FileQueryTests.test_triage_groups_repeated_anomalies_without_losing_counts_or_context", test_triage_groups_repeated_anomalies_without_losing_counts_or_context},
 {"FileQueryTests.test_triage_first_catch_info_context_recognizes_singular_plural_and_inline_json", test_triage_first_catch_info_context_recognizes_singular_plural_and_inline_json},
 {"FileQueryTests.test_triage_zero_hops_still_includes_bounded_counter_refinement", test_triage_zero_hops_still_includes_bounded_counter_refinement},
 {"FileQueryTests.test_triage_incomplete_end_is_an_anchor_without_claiming_failure", test_triage_incomplete_end_is_an_anchor_without_claiming_failure},
 {"FileQueryTests.test_triage_gaps_use_only_same_source_actual_clock_and_detect_regression", test_triage_gaps_use_only_same_source_actual_clock_and_detect_regression},
 {"FileQueryTests.test_triage_hops_are_capped_and_empty_ids_do_not_expand", test_triage_hops_are_capped_and_empty_ids_do_not_expand},
 {"FileQueryTests.test_triage_bounds_cardinality_payload_and_default_output_without_related_run", test_triage_bounds_cardinality_payload_and_default_output_without_related_run},
 {"FileQueryTests.test_triage_run_reuse_where_and_context_copies_are_not_false_duplicates", test_triage_run_reuse_where_and_context_copies_are_not_false_duplicates},
 {"FileQueryTests.test_triage_parse_damage_is_bounded_evidence_and_strict_remains_strict", test_triage_parse_damage_is_bounded_evidence_and_strict_remains_strict},
};
}  // namespace log_query_test
int mmltk::log_query::test_entry(int argc, char** argv) {
 using namespace log_query_test;
 try {
  if (argc > 1 && std::string_view(argv[1]) == "--fixture-runtime-docker") return runtime_docker(Strings(argv + 2, argv + argc));
  if (argc > 1 && std::string_view(argv[1]) == "--fixture-tools-docker") return tools_docker(Strings(argv + 2, argv + argc));
  if (argc > 1 && std::string_view(argv[1]) == "--fixture-package-docker") return package_docker(Strings(argv + 2, argv + argc));
  if (argc > 2 && std::string_view(argv[1]) == "--fixture-package-child") return package_child(Strings(argv + 2, argv + argc));
  if (argc == 3 && std::string_view(argv[1]) == "--fixture-inspector-self")
   return support::command::Invocation{
    support::command::Command::diagnose_native_process, {"--read-file", support::executable_path().filename().string(), "--pid", std::to_string(::getpid()), "--file", argv[2]}}
    .call(mmltk_tool_diagnose_native_process);
  if (argc == 2 && std::string_view(argv[1]) == "--fixture-tools-state") return tools_handoff_state();
  if (argc > 3 && std::string_view(argv[1]) == "--fixture-tools-user") {
   tools_fixture_user(argv[2]);
   ::execv(argv[3], argv + 3);
   throw std::system_error(errno, std::generic_category(), "exec fixture as tools user");
  }
  if (argc > 1 && std::string_view(argv[1]) == "--fixture-pause") {
   if (argc > 2 && std::string_view(argv[2]) == "ignore-term") ::signal(SIGTERM, SIG_IGN);
   std::cout << "ready" << std::endl;
   for (;;) ::pause();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--list-tests") return support::testing::list_cases(cases);
  const bool tools_ownership = argc == 2 && std::string_view(argv[1]) == "--tools-ownership";
  std::size_t failed = 0, ran = 0;
  for (const auto& test : cases) {
   bool selected = tools_ownership ? test.tools_ownership : argc == 1 && !test.tools_ownership;
   for (int index = 1; index < argc; ++index)
    selected = selected || test.name == argv[index] || (!test.tools_ownership && std::string_view(argv[index]).find('.') == std::string_view::npos && contains(test.name, argv[index]));
   if (!selected) continue;
   if (test.tools_ownership && !tools_ownership) throw Failure("tools ownership coverage requires ./mmltk --test log-query-tool --tools-ownership");
   ++ran;
   try {
    test.run();
    std::cout << "[ OK ] " << test.name << '\n';
   } catch (const std::exception& error) {
    ++failed;
    std::cerr << "[ FAILED ] " << test.name << ": " << error.what() << '\n';
   } catch (...) {
    ++failed;
    std::cerr << "[ FAILED ] " << test.name << ": unknown exception\n";
   }
  }
  if (!ran) {
   std::cerr << "No matching log-query cases\n";
   return 2;
  }
  std::cout << ran << " cases, " << failed << " failures\n";
  return failed ? 1 : 0;
 } catch (const std::exception& error) {
  std::cerr << "log-query native fixture: " << error.what() << '\n';
  return 2;
 }
}
