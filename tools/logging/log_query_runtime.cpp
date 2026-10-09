module;
// Public runtime definitions consume the private partition without publishing
// a second declaration of the public API through its compiled interface.
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
module mmltk.tools.log_query;
import std;
import :internal;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace mmltk::log_query {
using namespace detail;
bool missing(const Json& v) { return v.is_discarded(); }
std::string encoded(const Json& v) {
 if (missing(v)) return "null";
 if (v.is_binary()) return bigint(v).convert_to<std::string>();
 if (v.is_array()) {
  Strings s;
  for (const auto& x : v) s.push_back(encoded(x));
  return "[" + join(s, ",") + "]";
 }
 if (v.is_object()) {
  std::map<std::string, const Json*> items;
  for (auto it = v.begin(); it != v.end(); ++it) items[it.key()] = &it.value();
  Strings s;
  for (const auto& [k, x] : items) s.push_back(Json(k).dump(-1, ' ', true) + ":" + encoded(*x));
  return "{" + join(s, ",") + "}";
 }
 return v.dump(-1, ' ', true, Json::error_handler_t::replace);
}
bool exact(const Json& a, const Json& b) {
 if (missing(a) || missing(b)) return missing(a) && missing(b);
 if (a.is_boolean() || b.is_boolean()) return a.type() == b.type() && a == b;
 if (numeric(a) && numeric(b)) return comparison_number(a) == comparison_number(b);
 if (a.is_object() && b.is_object()) {
  if (a.size() != b.size()) return false;
  for (auto it = a.begin(); it != a.end(); ++it)
   if (!b.contains(it.key()) || !exact(it.value(), b[it.key()])) return false;
  return true;
 }
 if (a.is_array() && b.is_array()) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
   if (!exact(a[i], b[i])) return false;
  return true;
 }
 return a == b;
}
Json parse_json(std::string_view s) { return JsonReader(s).complete(); }
Json scalar(std::string_view s, bool quoted) {
 if (quoted) return unquote(s);
 if (s == "true") return true;
 if (s == "false") return false;
 if (s == "null") return nullptr;
 if (full(NUMBER, s)) {
  if (s.find_first_of(".eE") == std::string_view::npos) {
   if (s.size() - s.starts_with('-') > 4300) throw QueryError("numeric literal exceeds interpreter limits");
   return number(decimal_integer(s));
  }
  std::string token(s);
  char* end = nullptr;
  double value = std::strtod(token.c_str(), &end);
  if (end != token.c_str() + token.size() || !std::isfinite(value)) throw QueryError("numeric literals must be finite");
  return value;
 }
 return std::string(s);
}
std::string normalized_text(std::string_view value) {
 auto u = icu::UnicodeString::fromUTF8(icu::StringPiece(value.data(), static_cast<int32_t>(value.size())));
 icu::UnicodeString out;
 bool pending = false;
 for (int32_t i = 0; i < u.length();) {
  auto c = u.char32At(i);
  i += U16_LENGTH(c);
  if (u_isUWhiteSpace(c) || (c >= 0x1c && c <= 0x1f)) {
   pending = !out.isEmpty();
   continue;
  }
  if (pending) out.append(static_cast<UChar32>(' '));
  pending = false;
  out.append(c);
 }
 out.foldCase();
 std::string s;
 return out.toUTF8String(s);
}
std::string record_text(const Record& r) {
 if (observing("record.text")) observe("record.text", r.source);
 Strings values;
 std::vector<const Json*> pending;
 for (auto it = r.data.end(); it != r.data.begin();) pending.push_back(&*--it);
 while (!pending.empty()) {
  auto* v = pending.back();
  pending.pop_back();
  if (v->is_string())
   values.push_back(v->get<std::string>());
  else if (v->is_object() || v->is_array())
   for (auto it = v->end(); it != v->begin();) pending.push_back(&*--it);
 }
 return normalized_text(join(values, " "));
}
Json lookup(const Json& data, std::string_view name) {
 if (data.is_object()) {
  auto it = data.find(std::string(name));
  if (it != data.end()) return *it;
 }
 const Json* v = &data;
 for (const auto& p : tools::split(name, '.', true)) {
  if (v->is_object()) {
   auto it = v->find(p);
   if (it == v->end()) return MISSING;
   v = &*it;
  } else if (v->is_array()) {
   if (!full(R"(0|[1-9][0-9]*)", p)) return MISSING;
   std::size_t index = 0;
   auto [e, ec] = std::from_chars(p.data(), p.data() + p.size(), index);
   if (ec != std::errc() || index >= v->size()) return MISSING;
   v = &(*v)[index];
  } else
   return MISSING;
 }
 return *v;
}
Json value_from(const Json& data, std::initializer_list<std::string_view> names) {
 for (auto n : names) {
  auto v = lookup(data, n);
  if (present(v)) return v;
 }
 return MISSING;
}
Record::Record(std::string s, std::size_t l, std::string r, Json d, std::string f, std::string err, Json m)
    : source(std::move(s)), line(l), raw(std::move(r)), data(std::move(d)), format(std::move(f)), parse_error(std::move(err)), metadata(std::move(m)) {
 auto t = record_time(data, source);
 clock = std::move(t.first);
 time_ns = std::move(t.second);
}
bool Record::training_record() const {
 auto v = log_query::get(data, "format_version");
 if (!integer(v) || bigint(v) != 3 || !in(log_query::get(data, "role"), words("Live Boundary Epoch Terminal"))) return false;
 for (auto name : {"run_id", "attempt_id"}) {
  auto x = log_query::get(data, name);
  if (!x.is_string() || str(x).empty() || chars(str(x)) > 64 || trim(str(x)).empty()) return false;
 }
 return log_query::get(data, "progress").is_object() && in(lookup(data, "progress.phase"), words("Starting Train Validate EpochComplete Completed Error Cancelled Merge"));
}
Json Record::get(std::string_view name) const {
 if (observing("record.get")) observe("record.get", std::string(name));
 if (name == "@file") return source;
 if (name == "@line") return line;
 if (name == "@line_end") return log_query::get(metadata, "line_end", line);
 if (name == "@text") return raw;
 if (name == "@format") return format;
 if (name == "@clock") return clock;
 if (name == "@time_ns") return time_ns ? number(*time_ns) : MISSING;
 if (name == "@parse_error") return parse_error.empty() ? MISSING : Json(parse_error);
 if (name == "@surface") return surface_identity(data);
 if (name == "@workspace_source") return workspace_source_identity(data);
 if (name == "@process" || name == "@parent_process" || name == "@process_group") return process_identity(*this, name);
 if (name == "@intent_request") {
  auto e = get("@event");
  Json v;
  if (equals(e, "integration.intent"))
   v = value_from(data, {"control", "fields.control"});
  else if (in(e, words("browser.intent.received browser.intent.accepted browser.intent.rejected browser.intent.reply_rejected")))
   v = value_from(data, {"value", "fields.value"});
  else
   return MISSING;
  if (v.is_string() && full("[0-9]{1,20}", str(v))) v = number(decimal_integer(str(v)));
  return integer(v) && bigint(v) > 0 && bigint(v) < (Big(1) << 64) ? v : MISSING;
 }
 if (training_record()) {
  static const std::map<std::string, std::string> mapping = {
   {"@session", "progress.session_id"}, {"@model", "progress.model_id"}, {"@first_cause", "progress.failure.first_cause"}, {"@phase", "progress.phase"}, {"@role", "role"}
  };
  if (auto i = mapping.find(std::string(name)); i != mapping.end()) return lookup(data, i->second);
 }
 if (name == "@event") {
  if (training_record()) return "training." + lower(str(log_query::get(data, "role"))) + "." + lower(str(lookup(data, "progress.phase")));
  return value_from(data, {"fields.event", "fields.name", "event", "name"});
 }
 if (name == "@owner") {
  auto owner = value_from(data, {"owner", "fields.owner", "component", "system", "module", "logger"}), event = get("@event");
  return missing(owner) && event.is_string() ? Json(str(event).substr(0, str(event).find('.'))) : owner;
 }
 if (name == "@level") {
  auto v = value_from(data, {"level", "severity", "fields.level"});
  if (missing(v)) return MISSING;
  auto s = lower(str(v));
  static const std::map<std::string, std::string> mapping = {
   {"v", "trace"}, {"t", "trace"}, {"d", "debug"}, {"i", "info"}, {"w", "warning"}, {"warn", "warning"}, {"e", "error"}, {"err", "error"}, {"f", "fatal"}
  };
  auto i = mapping.find(s);
  return i == mapping.end() ? s : i->second;
 }
 if (name == "@error") {
  if (training_record()) return equals(log_query::get(data, "role"), "Terminal") && equals(lookup(data, "progress.phase"), "Error");
  auto code = get("@exit_code");
  if (!missing(code) && !exact(code, 0) && !exact(code, false)) return true;
  if (in(get("@level"), words("error critical fatal panic"))) return true;
  auto event = get("@event");
  if (format == "transcript" && (test_event(event) || in(event, words("build.progress build.source tap.summary tap.diagnostic libtest.summary libtest.output")))) return false;
  for (auto v : {event, value_from(data, {"message", "fields.message", "detail", "error"})})
   if (v.is_string() && search(FAILURE_WORD, str(v))) return true;
  return false;
 }
 if (name == "@exit_code") return in(get("@event"), words("child.signaled child.exited")) ? get("value") : get("exit_code");
 if (name == "@signal" || name == "@signal_number") {
  auto c = get("@exit_code");
  if (!equals(get("@event"), "child.signaled") || !integer(c)) return MISSING;
  Big n = bigint(c) - 128;
  if (n <= 0 || n > 64 || n == 32 || n == 33) return MISSING;
  int k = n.convert_to<int>();
  return name == "@signal_number" ? Json(k) : Json(signal_name(k));
 }
 if (name == "@terminal") {
  auto e = get("@event");
  return (training_record() && equals(log_query::get(data, "role"), "Terminal")) || (test_event(e) && !test_start(e)) ||
         in(e,
          words(
           "child.signaled child.exited shutdown.complete shutdown.firefox_terminal catch.test_selection_failed catch.summary tap.summary libtest.summary process.terminal build.image build.failed"));
 }
 if (name.starts_with('@')) return log_query::get(metadata, name.substr(1));
 auto v = lookup(data, name);
 if (missing(v) && name.find('.') == std::string_view::npos) v = lookup(data, "fields." + std::string(name));
 if (missing(v)) {
  Strings handles;
  for (const auto& [field, handle] : vulkan_resources(data))
   if (field == name) handles.push_back(handle);
  if (handles.size() == 1) return handles[0];
 }
 return v;
}
bool Expression::matches(const Record& r) const {
 if (operation == "all") return true;
 if (operation == "and") return std::all_of(children.begin(), children.end(), [&](const auto& e) { return e.matches(r); });
 if (operation == "or") return std::any_of(children.begin(), children.end(), [&](const auto& e) { return e.matches(r); });
 if (operation == "not") return !children.at(0).matches(r);
 if (operation == "text") {
  auto needle = lower(str(arguments[0]));
  if (lower(r.raw).find(needle) != std::string::npos) return true;
  for (auto n : {"@test", "@tags", "@run", "@archive_id", "@family", "@signal"}) {
   auto v = r.get(n);
   if (!missing(v) && lower(str(v)).find(needle) != std::string::npos) return true;
  }
  return false;
 }
 if (operation == "phrase" || operation == "words") {
  auto text = record_text(r);
  if (operation == "phrase") return std::any_of(arguments.begin(), arguments.end(), [&](const Json& s) { return text.find(str(s)) != std::string::npos; });
  return std::all_of(arguments.begin(), arguments.end(), [&](const Json& s) { return text.find(str(s)) != std::string::npos; });
 }
 auto v = r.get(str(arguments[0]));
 if (operation == "has") return !missing(v);
 if (missing(v)) return false;
 const auto& e = arguments[1];
 if (operation == "=" || operation == "!=") {
  bool equal = exact(v, e);
  return operation == "=" ? equal : !equal;
 }
 if (operation == ":") return lower(str(v)).find(lower(str(e))) != std::string::npos;
 if (operation == "~" || operation == "!~") {
  bool found = bool(search(str(e), str(v)));
  return operation == "~" ? found : !found;
 }
 if (!numeric(v) || v.is_boolean()) return false;
 int c = compare(v, e);
 return operation == ">" ? c > 0 : operation == ">=" ? c >= 0 : operation == "<" ? c < 0 : c <= 0;
}
QueryParser::QueryParser(std::string source) {
 if (chars(source) > MAX_QUERY_LENGTH) throw QueryError("query exceeds 8192 characters");
 std::size_t p = 0;
 const std::string pattern = R"rx(\s*(?:("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(!=|!~|>=|<=|[():=~<>])|([^\s():=~<>!"']+)))rx";
 while (!trim(std::string_view(source).substr(p)).empty()) {
  auto m = regex(pattern).search(source, p, true);
  if (!m) throw QueryError("unexpected character at column " + std::to_string(chars(source.substr(0, p)) + 1));
  tokens_.push_back({m.matched(1) ? m[1] : m.matched(2) ? m[2] : m[3], m.matched(1)});
  p = m.end();
  if (tokens_.size() > MAX_QUERY_TOKENS) throw QueryError("query exceeds 512 tokens");
 }
}
bool QueryParser::peek(std::string_view v) const { return index_ < tokens_.size() && !tokens_[index_].quoted && lower(tokens_[index_].text) == v; }
QueryParser::Token QueryParser::take() {
 if (index_ == tokens_.size()) throw QueryError("unexpected end of query");
 return tokens_[index_++];
}
void QueryParser::require(std::string_view v) {
 if (!peek(v)) throw QueryError("expected '" + std::string(v) + "' at token " + std::to_string(index_ + 1));
 ++index_;
}
Expression QueryParser::parse() {
 if (tokens_.empty()) return {};
 auto e = parse_or(0);
 if (index_ != tokens_.size()) throw QueryError("unexpected token '" + tokens_[index_].text + "'");
 return e;
}
Expression QueryParser::parse_or(std::size_t d) {
 std::vector<Expression> parts{parse_and(d)};
 while (peek("or")) {
  take();
  parts.push_back(parse_and(d));
 }
 return parts.size() == 1 ? parts[0] : Expression{"or", Json::array(), std::move(parts)};
}
Expression QueryParser::parse_and(std::size_t d) {
 std::vector<Expression> parts{primary(d)};
 while (index_ < tokens_.size() && !peek("or") && !peek(")")) {
  if (peek("and")) take();
  parts.push_back(primary(d));
 }
 return parts.size() == 1 ? parts[0] : Expression{"and", Json::array(), std::move(parts)};
}
Expression QueryParser::primary(std::size_t d) {
 if (d > MAX_QUERY_DEPTH) throw QueryError("query nesting exceeds 32");
 if (peek("not")) {
  take();
  return {"not", Json::array(), {primary(d + 1)}};
 }
 if (peek("(")) {
  take();
  auto e = parse_or(d + 1);
  require(")");
  return e;
 }
 if (peek("has")) {
  take();
  require("(");
  auto t = take();
  if (t.quoted || !full(FIELD_NAME, t.text)) throw QueryError("mmltk::tools::includes() requires a field name");
  require(")");
  return {"has", Json::array({t.text}), {}};
 }
 auto t = take();
 if (!t.quoted && mmltk::tools::includes(words("and or ) = : ~ !~ != > >= < <="), lower(t.text))) throw QueryError("expected a search term or field, received '" + t.text + "'");
 if (index_ < tokens_.size() && !tokens_[index_].quoted) {
  auto op = tokens_[index_].text;
  if (mmltk::tools::includes(words("= != : ~ !~ > >= < <="), op)) {
   if (t.quoted || !full(FIELD_NAME, t.text)) throw QueryError("comparisons require an unquoted field name");
   take();
   auto v = take();
   if (!v.quoted && mmltk::tools::includes(words("( ) = : ~ !~ != > >= < <="), v.text)) throw QueryError("comparison requires a value; quote literal punctuation");
   auto expected = scalar(v.text, v.quoted);
   if (mmltk::tools::includes(words("> >= < <="), op) && !numeric(expected)) throw QueryError(op + " requires a numeric value");
   if (op == "~" || op == "!~") {
    auto pattern = str(expected);
    std::string native_pattern;
    for (std::size_t k = 0; k < pattern.size(); ++k) {
     if (pattern[k] == '\\' && k + 1 < pattern.size()) {
      native_pattern += '\\';
      native_pattern += pattern[k + 1] == 'Z' ? 'z' : pattern[k + 1];
      ++k;
     } else
      native_pattern += pattern[k];
    }
    expected = native_pattern;
    regex(native_pattern);
   }
   return {op, Json::array({t.text, expected}), {}};
  }
 }
 return t.text == "*" && !t.quoted ? Expression{} : Expression{"text", Json::array({t.quoted ? scalar(t.text, true) : Json(t.text)}), {}};
}
Record parse_record(std::string source, std::size_t line, std::string raw, bool truncated) {
 auto clean = strip_ansi(rstrip_newline(raw));
 Json data = Json::object();
 std::string format = "text", message = clean;
 auto context = search(CONTEXT_LABEL, clean);
 auto payload = context ? clean.substr(context.end()) : clean;
 auto m = match(NATIVE, payload);
 if (m)
  format = "native";
 else if ((m = match(FIREFOX, payload)))
  format = "firefox";
 else if ((m = match(RUST, payload)))
  format = "rust";
 if (m) {
  data = m.fields();
  for (auto key : {"pid", "tid"})
   if (data.contains(key)) data[key] = number(decimal_integer(str(data[key])));
  message = str(data["message"]);
 }
 std::string error = truncated ? "line exceeds byte limit; text is truncated" : "";
 auto fatal = match(FATAL, message);
 if (fatal) {
  merge(data, {{"event", "fatal.report"}, {"level", "fatal"}, {"component", fatal["component"]}, {"message", message}, {"detail", fatal["detail"]}});
  auto envelope = match(FATAL_STATUS, fatal["detail"]);
  if (envelope) {
   for (auto key : {"operation", "status"}) data[key] = envelope[key];
   for (auto key : {"pid", "ppid", "pgid"}) data[key] = number(decimal_integer(envelope[key]));
   parse_pairs(data, trim(envelope["context"]), true);
  } else if (auto status = search(R"( \(status=([^()]*)\)$)", fatal["detail"]))
   data["status"] = status[1];
  return {source, line, clean, data, "fatal", error};
 }
 if (auto ve = vulkan_diagnostic(message, data)) return {source, line, rstrip_newline(raw), data, format, error.empty() ? *ve : error};
 auto transcript = transcript_data(clean);
 if (!transcript.is_null()) {
  merge(data, transcript);
  format = "transcript";
 } else if (message.find('{') != std::string::npos) {
  try {
   Json first = MISSING;
   bool malformed = false;
   std::vector<std::pair<std::size_t, Json>> objects;
   if ((source.ends_with(".jsonl") || source.ends_with(".json")) && trim(message).starts_with('{')) {
    try {
     first = parse_json(trim(message));
     if (!first.is_object()) throw QueryError("expected a JSON object");
    } catch (const QueryError&) {
     malformed = true;
     first = MISSING;
    }
   }
   if (missing(first)) {
    objects = embedded_objects(message);
    if (!objects.empty()) first = objects.front().second;
   }
   if (!missing(first)) {
    if (objects.size() > 1 && error.empty()) error = "multiple JSON payloads on one line; first retained";
    if (malformed && error.empty()) error = "extra or malformed content in JSONL record";
    merge(data, first);
    if (equals(get(data, "message"), message) && !first.contains("message")) data.erase("message");
    format = "json";
   } else if (trim(message).starts_with('{') || source.ends_with(".jsonl")) {
    if (error.empty()) error = "invalid or truncated JSON object";
   }
  } catch (const QueryError&) {
   if (error.empty()) error = "invalid or truncated JSON object";
  }
 } else if (source.ends_with(".jsonl") && error.empty())
  error = "expected a JSON object";
 if (format != "json" && format != "transcript") {
  if (!data.contains("message")) data["message"] = message;
  parse_pairs(data, message);
 }
 auto details = value_from(data, {"message", "fields.message"});
 if (details.is_string() && chars(str(details)) <= 4096 && trim(str(details)).starts_with('{')) {
  try {
   auto decoded = parse_json(str(details));
   if (decoded.is_object() && !data.contains("message_fields")) data["message_fields"] = std::move(decoded);
  } catch (const QueryError&) {}
 }
 return {std::move(source), line, std::move(clean), std::move(data), std::move(format), std::move(error)};
}
LogFile LogFile::capture(const fs::path& p, const fs::path& root) {
 if (observing("file.capture")) observe("file.capture", p.string());
 auto resolved = fs::canonical(p);
 if (!within(resolved, root)) throw QueryError("log input escapes repository mount: " + p.string());
 struct stat info{};
 if (::stat(resolved.c_str(), &info) != 0) throw QueryError("cannot inspect log file: " + p.string());
 if (!S_ISREG(info.st_mode)) throw QueryError("not a regular log file: " + p.string());
 LogFile f;
 f.path = resolved;
 f.source = resolved.lexically_relative(root).string();
 f.device = info.st_dev;
 f.inode = info.st_ino;
 f.size = info.st_size;
 f.modified_ns = Big(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
 return f;
}
LogFile LogFile::recent(std::size_t count) const {
 std::deque<std::uint64_t> starts{0};
 std::size_t lines = 1;
 std::uint64_t offset = 0;
 InputFile stream(path);
 check_capture(*this, stream.fd());
 while (offset < size) {
  auto b = stream.read(offset, static_cast<std::size_t>(std::min<std::uint64_t>(1024 * 1024, size - offset)));
  if (b.empty()) throw QueryError("log was truncated during query: " + source);
  for (std::size_t p = b.find('\n'); p != std::string::npos; p = b.find('\n', p + 1)) {
   auto next = offset + p + 1;
   if (next < size) {
    starts.push_back(next);
    if (starts.size() > count) starts.pop_front();
    ++lines;
   }
  }
  offset += b.size();
 }
 auto r = *this;
 r.start_offset = starts.front();
 r.first_line = lines - starts.size() + 1;
 return r;
}
void LogFile::records(const std::function<void(Record)>& visitor, bool unchanged, const Anchors& anchors, const std::function<bool(std::string_view)>& line_hint) const {
 if (observing("file.records")) observe("file.records", path.string());
 InputFile stream(path);
 check_capture(*this, stream.fd(), unchanged);
 Json m = {{"mtime_ns", number(modified_ns)}, {"run", source}, {"artifact", fs::path(source).stem().string()}};
 merge(m, metadata);
 TranscriptContext context;
 VulkanMessages messages;
 auto emit = [&](const Message& message) {
  if (line_hint && !line_hint(message.text) && !anchor_hint(message.text) && !match(TEST_STATUS, message.text) && !match(TAP_STATUS, message.text) && !match(LIBTEST_STATUS, message.text)) {
   auto t = trim(message.text);
   if (!t.starts_with("Filters:") && !t.starts_with("workspace-wayland:") && !t.starts_with("# Subtest:") && !t.starts_with("---- ")) return;
  }
  for (auto row : context.parse_line(source, message.first, message.text, m, anchors)) {
   if (!message.error.empty()) row.parse_error = message.error;
   if (message.last != message.first) row.metadata["line_end"] = message.last;
   visitor(std::move(row));
  }
 };
 if (path.extension() == ".json" && start_offset == 0) {
  auto raw = stream.read(0, static_cast<std::size_t>(std::min<std::uint64_t>(size, limits.line_bytes)));
  if (raw.size() != std::min<std::uint64_t>(size, limits.line_bytes)) throw QueryError("log was truncated during query: " + source);
  auto text = utf8(raw);
  auto row = parse_record(source, 1, text, size > limits.line_bytes);
  merge(row.metadata, m);
  row.metadata["line_end"] = std::max<std::size_t>(1, std::count(raw.begin(), raw.end(), '\n') + (!raw.ends_with('\n')));
  if (text != raw && row.parse_error.empty()) row.parse_error = "invalid UTF-8 bytes replaced";
  visitor(std::move(row));
  if (unchanged) check_capture(*this, stream.fd(), true);
  return;
 }
 std::uint64_t offset = start_offset;
 std::size_t line = first_line - 1;
 std::string retained;
 retained.reserve(std::min<std::uint64_t>(size - start_offset, limits.line_bytes));
 std::size_t physical_bytes = 0;
 auto flush = [&]() {
  ++line;
  bool truncated = physical_bytes > limits.line_bytes;
  std::string error = truncated ? "line exceeds byte limit; text is truncated" : "";
  auto text = utf8(retained);
  if (text != retained && error.empty()) error = "invalid UTF-8 bytes replaced";
  for (auto& msg : messages.push(line, text, error)) emit(msg);
  retained.clear();
  physical_bytes = 0;
 };
 while (offset < size) {
  auto block = stream.read(offset, static_cast<std::size_t>(std::min<std::uint64_t>(limits.line_bytes, size - offset)));
  if (block.empty()) throw QueryError("log was truncated during query: " + source);
  offset += block.size();
  std::size_t p = 0;
  while (p < block.size()) {
   auto nl = block.find('\n', p);
   auto end = nl == std::string::npos ? block.size() : nl + 1;
   auto amount = end - p;
   physical_bytes += amount;
   auto copy = std::min(amount, limits.line_bytes - retained.size());
   retained.append(block, p, copy);
   p = end;
   if (nl != std::string::npos) flush();
  }
 }
 if (physical_bytes) flush();
 if (auto last = messages.finish()) emit(*last);
 if (unchanged) check_capture(*this, stream.fd(), true);
}
Strings field_names(std::string_view value, char separator) {
 auto names = tools::split(value, separator, true);
 for (auto& n : names) {
  n = trim(n);
  if (!full(FIELD_NAME, n)) throw QueryError("invalid field list: '" + std::string(value) + "'");
 }
 if (names.empty()) throw QueryError("invalid field list: '" + std::string(value) + "'");
 return names;
}
std::optional<Strings> correlation_key(const Record& r, const Strings& names) {
 Strings parts;
 for (auto& n : names) {
  auto v = r.get(n);
  if (missing(v) || v.is_null() || v.is_boolean() || v.is_object() || v.is_array() || exact(v, 0) || equals(v, "") || (v.is_string() && full("0+", str(v)))) return {};
  parts.push_back(encoded(v));
 }
 return parts;
}
Json physical_position(const Record& r) { return Json::array({r.source, r.line, part(r)}); }
Json order_key(const Record& r, std::string_view order) {
 auto position = physical_position(r);
 if (order == "file") return position;
 if (order == "capture") return concatenate(Json::array({number(mtime(r))}), position);
 if (order == "proximity") return concatenate(Json::array({get(r.metadata, "proximity_ns", number(Big(1) << 63))}), position);
 int rank = r.clock == "steady" ? 0 : r.clock == "wall" ? 1 : r.clock == "wall-local" ? 2 : r.clock == "none" ? 4 : 3;
 return concatenate(Json::array({rank, r.clock, number(ns(r))}), position);
}
std::string Identity::label() const {
 Strings decoded;
 for (auto& v : values) decoded.push_back(compact(parse_json(v), 80));
 return compact(join(names, "+"), 128) + "=" + join(decoded, "+");
}
std::vector<Identity> strong_identities(const Record& r, const std::vector<Strings>& explicit_fields) {
 std::vector<Identity> out;
 auto run = str(get(r.metadata, "run", r.source));
 auto add = [&](Strings names, std::optional<Strings> values = {}) {
  auto key = values ? values : correlation_key(r, names);
  if (!key) return;
  for (auto& p : *key) {
   auto v = parse_json(p);
   if (v.is_string() && trim(str(v)).empty()) return;
  }
  Identity id{run, std::move(names), *key};
  mmltk::tools::append_unique(out, std::move(id));
 };
 add({"@surface"});
 add({"@workspace_source"});
 add({"@workspace_source", "transfer_sequence"});
 for (auto& names : explicit_fields) add(names);
 add({"source_session", "source_instance"});
 if (r.training_record()) {
  add({"run_id", "attempt_id"});
  add({"progress.session_id", "progress.model_id"});
  add({"progress.session_id", "progress.failure.first_cause"});
 }
 Json values = r.data;
 auto fields = get(r.data, "fields");
 if (fields.is_object()) merge(values, fields);
 for (auto it = values.begin(); it != values.end(); ++it) {
  if (out.size() >= 16) break;
  auto name = it.key();
  auto value = it.value();
  if (chars(name) > 128 || !full(FIELD_NAME, name) || mmltk::tools::includes(AMBIENT_IDS, name) || name == "surface_high" || name == "surface_low") continue;
  if (equals(r.get("@event"), "vulkan.validation") && (name == "validation_id" || name == "message_id")) continue;
  if (value.is_string() && chars(str(value)) > 128) continue;
  auto typed = value.is_string() ? match(TYPED_ID, str(value)) : Match{};
  if (typed && typed.end() == str(value).size()) {
   if (typed[1] == "DeviceId") continue;
   add({typed[1]}, Strings{encoded(typed[1] + "(" + replace_rx(" ", typed[2], "") + ")")});
  } else if (name.ends_with("_id") || name.ends_with("_identity")) {
   auto canonical = name.starts_with("parent_") ? name.substr(7) : name;
   auto key = correlation_key(r, {name});
   if (key) add({canonical}, key);
  } else if (name.ends_with("_surface") && value.is_string() && full("[0-9a-fA-F]{32}", str(value)) && Big("0x" + str(value)) != 0)
   add({"@surface"}, Strings{encoded(lower(str(value)))});
 }
 for (auto m : findall(TYPED_ID, prefix(r.raw, MAX_TRIAGE_TEXT))) {
  if (out.size() >= 16) break;
  if (m[1] != "DeviceId") add({m[1]}, Strings{encoded(m[1] + "(" + replace_rx(" ", m[2], "") + ")")});
 }
 return out;
}
std::pair<std::string, std::string> event_stage(const Record& r) {
 if (r.training_record()) return {};
 auto e = r.get("@event");
 auto m = e.is_string() ? match(STAGE_SUFFIX, str(e)) : Match{};
 return m ? std::pair{m[1], lower(m[2])} : std::pair<std::string, std::string>{};
}
bool triage_terminal(const Record& r) { return truth(r.get("@terminal")) || exact(r.get("terminal"), true) || event_stage(r).second == "terminal"; }
bool auto_correlate_enabled(const Options& o) {
 return o.auto_correlate != false && (o.auto_correlate == true || o.format == "timeline") && (!trim(o.query).empty() || o.errors) && o.correlate.empty() && !o.related_run && !o.error && !o.triage;
}
AutoFacts auto_facts(const Record& r) {
 std::map<std::string, std::string> facts;
 auto event = r.get("@event");
 for (auto& name : AUTO_FIELDS)
  if (auto v = auto_value(r.get(name))) facts[name] = *v;
 for (auto name : {"@surface", "@workspace_source"})
  if (auto v = auto_value(r.get(name))) facts[name] = *v;
 std::string domain;
 auto owner = r.get("@owner");
 if (in(owner, words("explore annotation upscale live"))) domain = str(owner);
 if (equals(event, "iced.gallery.source")) domain = "explore";
 if (event.is_string()) {
  auto i = AUTO_INTEGRATION_FIELDS.find(str(event));
  if (i != AUTO_INTEGRATION_FIELDS.end()) {
   domain = i->second.first;
   for (auto& [external, native] : i->second.second) {
    auto v = r.get(external);
    if (v.is_string() && full("[1-9][0-9]{0,15}", str(v))) v = number(decimal_integer(str(v)));
    if (numeric(v)) {
     auto n = rational(v);
     if (n > Rational(0) && n <= Rational(Big(1) << 53) && n.denominator() == 1) facts.try_emplace(native, encoded(number(n.numerator())));
    }
   }
  }
 }
 if (in(event, words("integration.phase_progress integration.phase_advanced")))
  if (auto v = auto_value(r.get("detail"))) facts.try_emplace("phase", *v);
 if (!domain.empty()) facts["domain"] = domain;
 for (auto [canonical, first, second] : {std::tuple{"frame", "source_revision", "frame_revision"}, {"snapshot", "source_observation_revision", "snapshot_revision"}}) {
  auto i = facts.find(first);
  if (i == facts.end()) i = facts.find(second);
  if (i != facts.end()) facts[canonical] = i->second;
 }
 std::string run = str(get(r.metadata, "run", r.source));
 std::vector<std::pair<int, Identity>> ids;
 auto add = [&](int priority, Strings names, std::optional<Strings> values = {}) {
  Strings key;
  if (values)
   key = *values;
  else
   for (auto& n : names) {
    auto it = facts.find(n);
    if (it == facts.end()) return;
    key.push_back(it->second);
   }
  std::pair<int, Identity> v{priority, {run, std::move(names), std::move(key)}};
  mmltk::tools::append_unique(ids, std::move(v));
 };
 if (process_evidence(r)) {
  for (auto n : {"@process", "@parent_process", "@process_group"}) {
   auto v = r.get(n);
   if (!missing(v)) facts[n] = encoded(v);
  }
  add(0, {"@process"});
  if (facts.contains("@parent_process")) add(0, {"@process"}, Strings{facts["@parent_process"]});
  add(1, {"@process_group"});
 }
 if (equals(event, "vulkan.validation") || in(event, VULKAN_PROVENANCE_EVENTS)) {
  auto process = vulkan_process(r);
  if (!missing(process)) {
   if (!process.is_null()) facts["browser_process_id"] = encoded(process);
   if (auto device = vulkan_handle(r.get("vk_device"))) facts["vk_device"] = encoded(*device);
   for (auto& [name, handle] : vulkan_resources(r.data)) {
    if (!process.is_null()) add(0, {"browser_process_id", name}, Strings{encoded(process), encoded(handle)});
    add(1, {"@file", name}, Strings{encoded(r.source), encoded(handle)});
   }
  }
 }
 if (in(event, MEMORY_PROVENANCE_EVENTS)) {
  add(0, {"native_process_id", "workspace_allocation"});
  add(0, {"browser_process_id", "@workspace_source", "workspace_allocation"});
 }
 for (auto field : {"frame", "snapshot"}) {
  add(0, {"source_session", "source_instance", field});
  if (!domain.empty() && facts.contains(field)) add(1, {domain + "." + field}, Strings{facts[field]});
 }
 for (auto field : {"presentation_revision", "allocation_generation"}) add(0, {"@surface", field});
 for (auto field : {"transfer_sequence", "timeline_ready", "workspace_allocation"}) add(0, {"@workspace_source", field});
 for (auto field : {"span_id", "request_id", "operation_id", "presentation_revision"}) add(2, {field});
 if (facts.contains("parent_span_id")) add(2, {"span_id"}, Strings{facts["parent_span_id"]});
 for (auto field : {"trace_id", "@surface", "@workspace_source", "dataset_identity", "gallery_identity"}) add(3, {field});
 add(1, {"dataset_identity", "gallery_generation"});
 if (!domain.empty() && facts.contains("observation_revision")) add(1, {domain + ".observation_revision"}, Strings{facts["observation_revision"]});
 for (auto field : {"phase", "phase_id"}) add(4, {field});
 auto control = r.get("control");
 if (control.is_string() && search("[./]", str(control))) add(5, {"control"});
 return {facts, ids};
}
std::string compact(const Json& v, std::optional<std::size_t> width) {
 if (missing(v)) return "(not recorded)";
 auto text = v.is_string() ? str(v) : encoded(v);
 auto u = icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(), static_cast<int32_t>(text.size())));
 icu::UnicodeString cleaned;
 for (int32_t i = 0; i < u.length();) {
  auto c = u.char32At(i);
  i += U16_LENGTH(c);
  std::string escaped;
  if (c == '\t')
   escaped = "\\t";
  else if (c == '\r')
   escaped = "\\r";
  else if (c == '\n')
   escaped = "\\n";
  else if (c != 0x20 && !u_isprint(c)) {
   std::ostringstream s;
   s << "\\x" << std::hex << std::setfill('0') << std::setw(2) << static_cast<std::uint32_t>(c);
   escaped = s.str();
  } else {
   cleaned.append(c);
   continue;
  }
  cleaned.append(icu::UnicodeString::fromUTF8(escaped));
 }
 std::string out;
 cleaned.toUTF8String(out);
 if (width && chars(out) > *width) return prefix(out, *width ? *width - 1 : 0) + "…";
 return out;
}
Json projected(const Record& r, const Strings& names) {
 Json out = {{"@file", r.source}, {"@line", r.line}};
 for (auto name : {"line_end", "representative_count"})
  if (r.metadata.contains(name)) out["@" + std::string(name)] = r.metadata[name];
 if (part(r)) out["@part"] = part(r);
 for (auto& name : names) {
  auto v = r.get(name);
  out[name] = missing(v) ? Json(nullptr) : v;
 }
 return out;
}
std::string render_record(const Retained& item, const Options& o) {
 const auto& r = item.record;
 if (o.format == "jsonl") {
  Json out;
  if (!o.fields.empty()) {
   out = projected(r, o.fields);
   out["@match"] = item.reason;
   if (item.reason == "auto") {
    out["@auto_reason"] = r.metadata["auto_reason"];
    out["@auto_anchor"] = r.metadata["auto_anchor"];
   }
  } else {
   Json log = {
    {"file", r.source}, {"line", r.line}, {"format", r.format}, {"clock", r.clock}, {"time_ns", r.time_ns ? number(*r.time_ns) : Json(nullptr)}, {"match", item.reason},
    {"parse_error", r.parse_error.empty() ? Json(nullptr) : Json(r.parse_error)}
   };
   merge(log, r.metadata);
   out = {{"_log", log}, {"data", r.data}, {"text", r.raw}};
  }
  return encoded(out);
 }
 std::string reason = item.reason == "auto" ? "auto: " + str(r.metadata["auto_reason"]) : item.reason;
 std::size_t detail_width = exact(r.get("@error"), true) ? 2048 : 180;
 auto event = r.get("@event");
 bool vulkan = equals(event, "vulkan.validation");
 if (!o.fields.empty()) {
  auto cells = projected(r, o.fields);
  Strings s;
  for (auto it = cells.begin(); it != cells.end(); ++it) s.push_back(it.key() + "=" + compact(it.value(), vulkan ? std::nullopt : std::optional<std::size_t>(detail_width)));
  return join(s, " ") + " [" + reason + "]";
 }
 auto clock = r.clock.starts_with("elapsed:") ? "elapsed" : r.clock;
 auto timestamp = clock == "elapsed" ? fixed_ms(ns(r)) + "ms" : r.time_ns ? r.time_ns->convert_to<std::string>() : "-";
 Strings labels;
 for (auto v : {r.get("@owner"), r.get("@level"), event})
  if (!missing(v)) labels.push_back(str(v));
 auto detail = value_from(r.data, {"message", "fields.message", "detail"});
 if (missing(detail) && r.training_record()) detail = r.get("progress.failure.detail");
 if (missing(detail) && missing(event)) detail = r.raw;
 Strings facts;
 auto append = [&](const Strings& names, std::optional<std::size_t> width) {
  for (auto& name : names) {
   auto v = r.get(name);
   if (!missing(v)) facts.push_back(name + "=" + compact(v, width));
  }
 };
 if (r.training_record())
  for (auto name : {"run_id", "attempt_id", "progress.session_id", "progress.model_id", "progress.failure.first_cause"}) facts.push_back(std::string(name) + "=" + compact(r.get(name), 70));
 if (r.metadata.contains("representative_count")) facts.push_back("occurrences=" + str(r.metadata["representative_count"]));
 if (vulkan) append(words("validation_id message_id api objects referenced_objects"), std::nullopt);
 for (auto name :
  words("trace_id span_id @surface @workspace_source source_session source_instance source_revision frame_revision presentation_revision allocation_generation workspace_allocation workspace_plane"))
  if (correlation_key(r, {name})) facts.push_back(name + "=" + compact(r.get(name), 70));
 append(words("participant reason observed_event observed_file observed_line audit_context.field"), 150);
 auto es = str(event);
 if (NATIVE_PIXEL_STAGES.contains(es) || DIRECT_PIXEL_STAGES.contains(es) ||
     mmltk::tools::includes(words("firefox.workspace.direct_memory_alias firefox.workspace.direct_memory_status firefox.workspace.direct_memory_pixel"), es)) {
  append(words("transfer_sequence timeline_ready memory_alias_supported sample_index sample_x sample_y sample_byte_offset sample_rgba completion readback_marker"), 70);
  if (es == "firefox.workspace.direct_memory_pixel") facts.push_back("pixel_evidence=undefined_alias_after_image_acquire");
 }
 if (in(event, MEMORY_PROVENANCE_EVENTS) || es == "firefox.workspace.direct_binding") {
  append(MEMORY_PROVENANCE_FIELDS, 100);
  if (es == "firefox.workspace.direct_binding") append(words("phase storage_image chosen_layout transfer_sequence texture_id"), 100);
 }
 if (in(event, LIBRARY_PROVENANCE_EVENTS)) {
  for (auto name :
   words("native_process_id browser_process_id library_path library_provenance inventory_status inventory_errno library_count maps_bytes maps_byte_limit inventory_bounded path_encoding_unavailable"))
   append({name}, name == "library_path" ? std::nullopt : std::optional<std::size_t>(100));
 }
 for (auto name : {"@signal", "@exit_code", "@test", "buffer"}) {
  if (std::string_view(name) == "@test" && o.error) continue;
  append({name}, 90);
 }
 if (event.is_string() && search(HANDOFF_EVENT, es)) append(words("sequence value"), 70);
 if (es == "presentation.workspace.wait" || es == "presentation.workspace.service")
  append(words("requested_product_owner requested_product_revision observed_product_owner observed_product_revision admitted_allocation admitted_access admitted_generation admitted_display_held "
               "admitted_write_reserved admitted_completion_pending source_timeline_imported workspace_admitted workspace_write_available"),
   70);
 if (o.triage) {
  append(words("generation slot boundary status code outcome span_outcome"), 70);
  if (triage_terminal(r)) {
   auto fields = get(r.data, "fields");
   const auto& values = fields.is_object() ? fields : r.data;
   int emitted = 0;
   for (auto it = values.begin(); it != values.end(); ++it) {
    const auto& value = it.value();
    if (mmltk::tools::includes(words("event name owner level timestamp steady_ns elapsed_ms status code outcome span_outcome"), it.key()) || value.is_null() || equals(value, "") || exact(value, 0) ||
        exact(value, false) || !(value.is_string() || numeric(value) || value.is_boolean()))
     continue;
    facts.push_back(compact(it.key(), 70) + "=" + compact(value, 70));
    if (++emitted == 6) break;
   }
  }
 }
 append(words("strong_count settled"), 70);
 if (meta(r, "context_copy")) facts.push_back("context-copy");
 if (r.metadata.contains("proximity_ns")) facts.push_back("delta_ms=" + fixed_ms(bigint(r.metadata["proximity_ns"]), true) + " (" + str(r.metadata["time_link"]) + ")");
 std::string suffix = !missing(detail) && !equals(detail, "") && !vulkan ? " " + compact(detail, detail_width) : "";
 if (!facts.empty()) suffix += " " + join(facts, " ");
 auto location = std::to_string(r.line);
 if (r.metadata.contains("line_end")) location += "-" + str(r.metadata["line_end"]);
 auto rendered = compact(clock, 70) + ":" + timestamp + " " + compact(r.source, 150) + ":" + location + " [" + reason + "] " + compact(join(labels, " ")) + suffix;
 if (vulkan) {
  for (auto& line : tools::lines(r.raw)) rendered += "\n    " + compact(line, std::nullopt);
 }
 return rendered;
}
Options parse_options(const Strings& args) {
 Options o;
 std::optional<bool> automatic;
 bool positional = false;
 for (std::size_t i = 0; i < args.size(); ++i) {
  auto arg = args[i];
  if (positional) {
   o.paths.push_back(arg);
   continue;
  }
  if (arg == "--") {
   positional = true;
   continue;
  }
  if (arg.empty() || arg[0] != '-' || arg == "-") {
   o.paths.push_back(arg);
   continue;
  }
  std::optional<std::string> inline_value;
  auto eq = arg.find('=');
  if (eq != std::string::npos && arg.starts_with("--")) {
   inline_value = arg.substr(eq + 1);
   arg.resize(eq);
  } else if (arg.starts_with("-q") && arg != "-q") {
   inline_value = arg.substr(2);
   arg = "-q";
  }
  auto value = [&]() {
   if (inline_value) return *inline_value;
   if (i + 1 >= args.size() || (args[i + 1].starts_with("-") && !full(NUMBER, args[i + 1]))) throw QueryError("argument " + arg + ": expected one argument");
   return args[++i];
  };
  auto flag = [&]() {
   if (inline_value) throw QueryError("argument " + arg + ": ignored explicit argument '" + *inline_value + "'");
  };
  if (arg == "-h" || arg == "--help") {
   flag();
   o.help = true;
  } else if (arg == "-q" || arg == "--query")
   o.query = value();
  else if (arg == "--where")
   o.where = value();
  else if (arg == "--error")
   o.error = value();
  else if (arg == "--correlate")
   o.correlate.push_back(value());
  else if (arg == "--family")
   o.family.push_back(value());
  else if (arg == "--run")
   o.run = value();
  else if (arg == "--fields") {
   auto fields = value();
   o.fields = fields.empty() ? Strings{} : field_names(fields);
  } else if (arg == "--group-by")
   o.group_by = field_names(value());
  else if (arg == "--stats") {
   for (auto& name : field_names(value())) mmltk::tools::append_unique(o.stats, name);
  } else if (arg == "--format") {
   o.format = value();
   if (!mmltk::tools::includes(words("summary timeline jsonl"), o.format)) throw QueryError("argument --format: invalid choice: '" + o.format + "'");
  } else if (arg == "--order") {
   o.order = value();
   if (!mmltk::tools::includes(words("time file capture proximity"), o.order)) throw QueryError("argument --order: invalid choice: '" + o.order + "'");
  } else if (arg == "--triage-anchors")
   o.triage_anchors = bounded(value(), 1, 12);
  else if (arg == "--triage-identities")
   o.triage_identities = bounded(value(), 1, 64);
  else if (arg == "--triage-hops")
   o.triage_hops = bounded(value(), 0, 3);
  else if (arg == "--triage-gap-ms")
   o.triage_gap_ms = bounded(value(), 1, 600000);
  else if (arg == "--near-ms")
   o.near_ms = bounded(value(), 0, 600000);
  else if (arg == "--context")
   o.context = bounded(value(), 0, 100);
  else if (arg == "--bucket-ms")
   o.bucket_ms = bounded(value(), 1, 86400000);
  else if (arg == "--top")
   o.top = bounded(value(), 1, 100);
  else if (arg == "--limit")
   o.limit = bounded(value(), 1, 10000);
  else if (arg == "--last-lines")
   o.last_lines = bounded(value(), 1, 100000);
  else {
   flag();
   if (arg == "--errors")
    o.errors = true;
   else if (arg == "--triage")
    o.triage = true;
   else if (arg == "--descriptor-lineage")
    o.descriptor_lineage = true;
   else if (arg == "--representatives")
    o.representatives = true;
   else if (arg == "--tail")
    o.tail = true;
   else if (arg == "--recursive")
    o.recursive = true;
   else if (arg == "--history")
    o.history = true;
   else if (arg == "--list-runs")
    o.list_runs = true;
   else if (arg == "--related-run")
    o.related_run = true;
   else if (arg == "--strict")
    o.strict = true;
   else if (arg == "--auto-correlate" || arg == "--no-auto-correlate") {
    bool v = arg == "--auto-correlate";
    if (automatic && *automatic != v) throw QueryError("argument " + arg + ": not allowed with argument " + (*automatic ? "--auto-correlate" : "--no-auto-correlate"));
    automatic = v;
    o.auto_correlate = v;
   } else
    throw QueryError("unrecognized arguments: " + arg);
  }
 }
 return o;
}
int main(const Strings& args, const fs::path& root, std::ostream& out, std::ostream& diagnostics) {
 try {
  Options o = parse_options(args);
  if (o.help) {
   render_help(out);
   return 0;
  }
  auto query = QueryParser(o.query).parse(), where = QueryParser(o.where).parse();
  if (o.errors) query = {"and", Json::array(), {query, {"=", Json::array({"@error", true}), {}}}};
  if (o.descriptor_lineage) {
   if (trim(o.query).empty() || o.triage || o.related_run || o.error || o.list_runs || o.representatives)
    throw QueryError("--descriptor-lineage requires --query and cannot combine with --triage, --related-run, --error, --list-runs, or --representatives");
   Expression events{"or", Json::array(), {}};
   for (auto& e : MEMORY_PROVENANCE_EVENTS) events.children.push_back({"=", Json::array({"@event", e}), {}});
   where = {"and", Json::array(), {where, events}};
   for (auto names :
    {"native_process_id+workspace_allocation", "browser_process_id+source+workspace_allocation", "native_process_id+workspace_descriptor+workspace_plane", "browser_process_id+vk_device+vk_image"})
    mmltk::tools::append_unique(o.correlate, names);
  }
  if (o.triage && (o.related_run || o.tail || o.list_runs)) throw QueryError("--triage cannot combine with --related-run, --tail, or --list-runs; its evidence is bounded automatically");
  if (o.representatives && (o.triage || o.tail || o.error)) throw QueryError("--representatives cannot combine with --triage, --tail, or --error");
  if (!o.correlate.empty() && trim(o.query).empty() && !o.errors && !o.triage) throw QueryError("--correlate requires an explicit --query or --errors seed");
  if (o.auto_correlate == true && !auto_correlate_enabled(o))
   throw QueryError("--auto-correlate requires --query or --errors and cannot combine with --correlate, --related-run, --error, or --triage");
  if (o.stats.size() > 32) throw QueryError("--stats accepts at most 32 distinct fields");
  if ((!o.stats.empty() || o.bucket_ms) && (o.triage || o.list_runs)) throw QueryError("statistics require the streaming query and cannot combine with --triage or --list-runs");
  if (!o.top) o.top = o.triage ? 5 : 10;
  if (o.order.empty()) o.order = o.error ? "proximity" : "time";
  if (o.order == "proximity" && !o.error) throw QueryError("--order proximity requires --error");
  if (o.error) o.history = true;
  for (auto& relationship : o.correlate) field_names(relationship, '+');
  auto repository = fs::canonical(root.empty() ? tools::repository_root({}, __FILE__) : root);
  ArtifactCatalog catalog(o, repository);
  if (o.list_runs) {
   catalog.list_runs(out, o.limit);
   return 0;
  }
  if (o.last_lines) {
   for (auto& f : catalog.files) f = f.recent(o.last_lines);
   diagnostics << "Partial input: last " << o.last_lines << " physical lines per file; earlier context and evidence excluded.\n";
   for (auto& f : catalog.files) diagnostics << "  " << f.source << ": starting at line " << f.first_line << ", byte " << f.start_offset << '\n';
  }
  if (!o.family.empty() || o.history || !o.run.empty() || o.related_run || o.triage) catalog.prepare_anchors();
  if (!catalog.selected_runs.empty()) {
   Expression scope{"or", Json::array(), {}};
   for (auto& run : catalog.selected_runs) scope.children.push_back({"=", Json::array({"@run", run}), {}});
   where = {"and", Json::array(), {where, scope}};
  }
  std::unique_ptr<ErrorLookup> lookup;
  auto files = catalog.files;
  if (o.error) {
   lookup = std::make_unique<ErrorLookup>(*o.error, o);
   lookup->locate(files, query, where, catalog.anchors);
   if (!lookup->focus) {
    out << "No exact/display-normalized phrase or all-words matches in " << files.size() << " files.\n";
    if (o.strict && lookup->malformed) {
     diagnostics << "Strict lookup scan observed " << lookup->malformed << " malformed/truncated records.\n";
     return 2;
    }
    return 1;
   }
   std::erase_if(files, [&](const LogFile& f) { return !lookup->relevant(f); });
   lookup->calibrate(files, catalog.anchors);
   lookup->annotate(*lookup->focus, true);
   query = lookup->expression;
   where = {"and", Json::array(), {where, {"=", Json::array({"@run", lookup->focus->metadata["run"]}), {}}}};
  }
  if (o.triage) {
   Triage triage(o, where, catalog.anchors, lookup.get());
   triage.run_query(files, query);
   render_triage(triage, o, out, diagnostics);
   if (o.strict && (triage.malformed || (lookup && lookup->malformed))) return 2;
   return triage.selected.empty() ? 1 : 0;
  }
  auto result = execute(files, query, where, o, catalog.anchors, lookup.get());
  render(result, o, out, diagnostics);
  if (o.strict && lookup && lookup->malformed) {
   if (lookup->malformed != result.malformed) diagnostics << "Strict lookup scan observed " << lookup->malformed << " malformed/truncated records across all inputs.\n";
   return 2;
  }
  if (o.strict && result.malformed) return 2;
  return result.matches + result.related ? 0 : 1;
 } catch (const std::exception& error) {
  diagnostics << "mmltk logs: " << error.what() << '\n';
  return 2;
 }
}
Record triage_snapshot(const Record& r) {
 int budget = 96;
 bool shortened = false, training = r.training_record();
 std::function<Json(const Json&, int, std::string_view)> trim_value = [&](const Json& v, int depth, std::string_view path) -> Json {
  if (v.is_string()) {
   auto s = str(v);
   if (chars(s) > MAX_TRIAGE_TEXT) {
    shortened = true;
    return prefix(s, MAX_TRIAGE_TEXT) + "…";
   }
   return v;
  }
  if (v.is_array() || v.is_object()) {
   if (depth >= 4 || budget <= 0) {
    shortened = true;
    return "<triage payload omitted>";
   }
   Json result = v.is_object() ? Json::object() : Json::array();
   std::vector<std::pair<std::string, const Json*>> items;
   if (v.is_object()) {
    Strings priority;
    if (training && path.empty())
     priority = words("format_version run_id attempt_id role sequence progress");
    else if (training && path == "progress")
     priority = words("phase scope session_id model_id failure");
    for (auto& n : priority)
     if (v.contains(n)) items.emplace_back(n, &v[n]);
    std::vector<std::pair<std::string, const Json*>> rest;
    int inspected = 0;
    for (auto i = v.begin(); i != v.end() && inspected <= budget; ++i, ++inspected)
     if (!mmltk::tools::includes(priority, i.key())) rest.emplace_back(i.key(), &i.value());
    std::stable_sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) { return (a.second->is_object() || a.second->is_array()) < (b.second->is_object() || b.second->is_array()); });
    items.insert(items.end(), rest.begin(), rest.end());
   } else {
    for (std::size_t i = 0; i < v.size() && i <= static_cast<std::size_t>(budget); ++i) items.emplace_back(std::to_string(i), &v[i]);
   }
   for (auto& [name, child] : items) {
    if (budget <= 0) {
     shortened = true;
     break;
    }
    --budget;
    auto value = trim_value(*child, depth + 1, path.empty() ? name : std::string(path) + "." + name);
    if (v.is_object()) {
     std::string key = name;
     if (chars(key) > MAX_TRIAGE_TEXT) {
      key = prefix(key, MAX_TRIAGE_TEXT) + "…";
      shortened = true;
     }
     result[key] = std::move(value);
    } else
     result.push_back(std::move(value));
   }
   return result;
  }
  return v;
 };
 auto out = r;
 out.raw = str(trim_value(r.raw, 0, "raw"));
 out.data = trim_value(r.data, 0, "");
 if (shortened) out.metadata["triage_payload_truncated"] = true;
 return out;
}
}  // namespace mmltk::log_query
int mmltk::log_query::entry(int argc, char** argv) {
 std::signal(SIGPIPE, SIG_IGN);
 std::vector<std::string> args(argv + 1, argv + argc);
 int status = mmltk::log_query::main(args, {}, std::cout, std::cerr);
 std::cout.flush();
 if (!std::cout && errno == EPIPE) return 0;
 return status;
}
