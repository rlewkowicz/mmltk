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
#include <type_traits>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
export module mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
export {
 namespace mmltk::tools {
 namespace fs = std::filesystem;
 using json = nlohmann::ordered_json;
 class ToolError : public std::runtime_error {
 public:
  explicit ToolError(std::string message, int status = 1) : std::runtime_error(std::move(message)), status(status) {}
  int status;
 };
 inline std::optional<std::string> optional_env(std::string_view name) {
  const std::string key(name);
  if (const char* value = std::getenv(key.c_str())) return std::string(value);
  return std::nullopt;
 }
 inline std::string env(std::string_view name, std::string_view fallback = {}) {
  const auto value = optional_env(name);
  return value ? *value : std::string(fallback);
 }
 using Environment = std::map<std::string, std::string>;
 inline Environment current_environment() {
  Environment result;
  for (char** item = ::environ; item && *item; ++item) {
   const std::string_view value(*item);
   const auto equal = value.find('=');
   if (equal != std::string_view::npos) result.emplace(value.substr(0, equal), value.substr(equal + 1));
  }
  return result;
 }
 template <class Error = ToolError>
 class CommandEnvironment {
 public:
  explicit CommandEnvironment(const Environment& overrides, const std::vector<std::string>& unset = {}, bool inherit = true) {
   if (inherit)
    for (char** entry = ::environ; entry && *entry; ++entry) {
     const std::string_view item(*entry);
     const std::string name(item.substr(0, item.find('=')));
     if (!overrides.contains(name) && std::find(unset.begin(), unset.end(), name) == unset.end()) values_.emplace_back(item);
    }
   for (const auto& [name, value] : overrides) {
    if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) throw Error("invalid command environment entry");
    values_.push_back(name + '=' + value);
   }
   pointers_.reserve(values_.size() + 1);
   for (auto& value : values_) pointers_.push_back(value.data());
   pointers_.push_back(nullptr);
  }
  CommandEnvironment(const CommandEnvironment&) = delete;
  CommandEnvironment& operator=(const CommandEnvironment&) = delete;
  char* const* data() const { return pointers_.data(); }

 private:
  std::vector<std::string> values_;
  std::vector<char*> pointers_;
 };
 class ScopedEnvironment {
 public:
  explicit ScopedEnvironment(std::string name) : name_(std::move(name)), previous_(optional_env(name_)) {}
  ScopedEnvironment(std::string name, const std::string& value) : ScopedEnvironment(std::move(name)) {
   if (::setenv(name_.c_str(), value.c_str(), 1) != 0) throw std::system_error(errno, std::generic_category(), "setenv " + name_);
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
  ~ScopedEnvironment() {
   if (previous_)
    ::setenv(name_.c_str(), previous_->c_str(), 1);
   else
    ::unsetenv(name_.c_str());
  }

 private:
  std::string name_;
  std::optional<std::string> previous_;
 };
 inline bool path_within(const fs::path& path, const fs::path& root) {
  auto part = path.begin();
  for (const auto& component : root) {
   if (part == path.end() || *part != component) return false;
   ++part;
  }
  return true;
 }
 inline bool regular_file(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
 }
 template <class Range, class Value>
 bool includes(const Range& range, const Value& value) {
  return std::find(range.begin(), range.end(), value) != range.end();
 }
 template <class T>
 void append_unique(std::vector<T>& values, std::type_identity_t<T> value) {
  if (!includes(values, value)) values.push_back(std::move(value));
 }
 struct ReadResult {
  std::size_t size;
  int error;
 };
 inline ReadResult read_into(int fd, std::span<char> buffer) noexcept {
  std::size_t used = 0;
  while (used < buffer.size()) {
   const auto count = ::read(fd, buffer.data() + used, buffer.size() - used);
   if (count < 0 && errno == EINTR) continue;
   if (count < 0) return {used, errno};
   if (count == 0) break;
   used += static_cast<std::size_t>(count);
  }
  return {used, 0};
 }
 inline int try_write_all(int fd, std::string_view bytes) noexcept {
  while (!bytes.empty()) {
   const auto count = ::write(fd, bytes.data(), bytes.size());
   if (count < 0 && errno == EINTR) continue;
   if (count <= 0) return count < 0 ? errno : EIO;
   bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return 0;
 }
 inline void write_all(int fd, std::string_view bytes, const std::string& context) {
  if (const int error = try_write_all(fd, bytes)) throw std::system_error(error, std::generic_category(), context);
 }
 inline std::string trim(std::string_view value) {
  constexpr std::string_view whitespace = " \t\r\n\f\v";
  const auto first = value.find_first_not_of(whitespace);
  if (first == std::string_view::npos) return {};
  return std::string(value.substr(first, value.find_last_not_of(whitespace) - first + 1));
 }
 inline std::vector<std::string> split(std::string_view value, char separator, bool keep_empty = true) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (true) {
   const auto end = value.find(separator, start);
   const auto part = value.substr(start, end == std::string_view::npos ? end : end - start);
   if (keep_empty || !part.empty()) result.emplace_back(part);
   if (end == std::string_view::npos) break;
   start = end + 1;
  }
  return result;
 }
 inline std::vector<std::string> lines(std::string_view value) {
  if (value.empty()) return {};
  auto result = split(value, '\n');
  if (value.back() == '\n') result.pop_back();
  for (auto& line : result)
   if (!line.empty() && line.back() == '\r') line.pop_back();
  return result;
 }
 template <class Attempt>
 int search_executable(std::string_view path, std::string_view command, Attempt&& attempt) {
  int error = ENOENT;
  bool denied = false;
  for (const auto& directory : split(path, ':')) {
   error = attempt(fs::path(directory) / command);
   if (!error) break;
   if (error == EACCES)
    denied = true;
   else if (error != ENOENT && error != ENOTDIR)
    break;
  }
  return (error == ENOENT || error == ENOTDIR) && denied ? EACCES : error;
 }
 inline fs::path lexical_path(std::string_view value, std::string_view empty = ".", bool double_root = true) {
  std::string result;
  if (double_root && value.starts_with("//") && !value.starts_with("///"))
   result = "//";
  else if (value.starts_with('/'))
   result = "/";
  for (const auto& part : split(value, '/', false)) {
   if (part == ".") continue;
   if (!result.empty() && result.back() != '/') result += '/';
   result += part;
  }
  return result.empty() ? fs::path(empty) : fs::path(result);
 }
 inline std::string read_text(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw ToolError("cannot read " + path.string());
  std::string result{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (stream.bad()) throw ToolError("read failed: " + path.string());
  return result;
 }
 inline void write_text(const fs::path& path, std::string_view text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw ToolError("cannot write " + path.string());
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.close();
  if (!stream) throw ToolError("write failed: " + path.string());
 }
 inline json read_json(const fs::path& path) { return json::parse(read_text(path)); }
 // Template-string conversion used by the JavaScript tool counterparts.
 inline std::string js_string(const json& value) {
  if (value.is_string()) return value.get<std::string>();
  if (value.is_object()) return "[object Object]";
  if (value.is_array()) {
   std::string result;
   bool first = true;
   for (const auto& element : value) {
    if (!first) result += ',';
    first = false;
    if (!element.is_null()) result += js_string(element);
   }
   return result;
  }
  return value.dump();
 }
 // JavaScript's non-Unicode lexical rules can retain an isolated UTF-16
 // surrogate as a token. Such internal strings use WTF-8; JSON.stringify emits
 // the corresponding \u escape. Normal UTF-8 takes nlohmann's ordinary path.
 inline std::string json_dump(const json& value, int indent = -1) {
  try {
   return value.dump(indent);
  } catch (const json::type_error& error) {
   if (error.id != 316) throw;
  }
  const auto quote = [](std::string_view text) {
   std::string result = "\"";
   std::size_t start = 0;
   const auto append_valid = [&](std::string_view part) {
    const auto escaped = json(std::string(part)).dump();
    result.append(escaped, 1, escaped.size() - 2);
   };
   constexpr char hex[] = "0123456789abcdef";
   for (std::size_t index = 0; index + 2 < text.size(); ++index) {
    const auto first = static_cast<unsigned char>(text[index]);
    const auto second = static_cast<unsigned char>(text[index + 1]);
    const auto third = static_cast<unsigned char>(text[index + 2]);
    if (first != 0xed || second < 0xa0 || second > 0xbf || third < 0x80 || third > 0xbf) continue;
    append_valid(text.substr(start, index - start));
    const unsigned code = ((first & 0x0fU) << 12) | ((second & 0x3fU) << 6) | (third & 0x3fU);
    result += "\\u";
    for (int shift = 12; shift >= 0; shift -= 4) result += hex[(code >> shift) & 0x0fU];
    index += 2;
    start = index + 1;
   }
   append_valid(text.substr(start));
   result += '"';
   return result;
  };
  const auto render = [&](const auto& self, const json& node, std::size_t depth) -> std::string {
   if (node.is_string()) return quote(node.get_ref<const std::string&>());
   if (!node.is_array() && !node.is_object()) return node.dump();
   const bool object = node.is_object();
   std::string result(1, object ? '{' : '[');
   if (!node.empty()) {
    bool first = true;
    for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
     if (!first) result += ',';
     first = false;
     if (indent >= 0) {
      result += '\n';
      result.append((depth + 1) * static_cast<std::size_t>(indent), ' ');
     }
     if (object) {
      result += quote(iterator.key());
      result += indent >= 0 ? ": " : ":";
     }
     result += self(self, iterator.value(), depth + 1);
    }
    if (indent >= 0) {
     result += '\n';
     result.append(depth * static_cast<std::size_t>(indent), ' ');
    }
   }
   result += object ? '}' : ']';
   return result;
  };
  return render(render, value, 0);
 }
 inline void write_json(const fs::path& path, const json& value, int indent = 2) { write_text(path, json_dump(value, indent) + "\n"); }
 inline std::string shell_quote(std::string_view value) {
  std::string result("'");
  for (const char ch : value) {
   if (ch == '\'')
    result += "'\\''";
   else
    result += ch;
  }
  result += '\'';
  return result;
 }
 // POSIX shlex.split, with comments disabled, for compilation databases that
 // carry a command string instead of an already separated argument vector.
 inline std::vector<std::string> shell_words(std::string_view command) {
  std::vector<std::string> arguments;
  std::string word;
  char quote = '\0';
  bool started = false;
  for (std::size_t index = 0; index < command.size(); ++index) {
   const char ch = command[index];
   if (quote == '\'') {
    if (ch == '\'')
     quote = '\0';
    else
     word += ch;
   } else if (ch == '\\') {
    if (++index == command.size()) throw ToolError("No escaped character in compile command");
    const char escaped = command[index];
    if (quote == '"' && escaped != '"' && escaped != '\\') word += '\\';
    word += escaped;
    started = true;
   } else if (quote == '"') {
    if (ch == '"')
     quote = '\0';
    else
     word += ch;
   } else if (ch == '\'' || ch == '"') {
    quote = ch;
    started = true;
   } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
    if (started) arguments.push_back(std::exchange(word, {}));
    started = false;
   } else {
    word += ch;
    started = true;
   }
  }
  if (quote) throw ToolError("No closing quotation in compile command");
  if (started) arguments.push_back(std::move(word));
  return arguments;
 }
 inline fs::path executable_path() {
  std::vector<char> buffer(256);
  while (true) {
   const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
   if (count < 0) throw std::system_error(errno, std::generic_category(), "readlink /proc/self/exe");
   if (static_cast<std::size_t>(count) < buffer.size()) return std::string(buffer.data(), static_cast<std::size_t>(count));
   buffer.resize(buffer.size() * 2);
  }
 }
 inline fs::path repository_root(std::string_view argv0, std::string_view source_file = {}) {
  std::vector<fs::path> candidates;
  if (!source_file.empty()) candidates.emplace_back(fs::absolute(fs::path(source_file)).parent_path());
  if (!argv0.empty()) candidates.emplace_back(fs::absolute(fs::path(argv0)).parent_path());
  candidates.push_back(executable_path().parent_path());
  candidates.push_back(fs::current_path());
  for (auto candidate : candidates) {
   candidate = fs::weakly_canonical(candidate);
   while (!candidate.empty()) {
    if (fs::exists(candidate / "mmltk") && fs::is_directory(candidate / "tools")) return candidate;
    const auto parent = candidate.parent_path();
    if (parent == candidate) break;
    candidate = parent;
   }
  }
  throw ToolError("cannot locate repository containing mmltk and tools");
 }
 class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
  ~FileDescriptor() { reset(); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
   if (this != &other) reset(other.release());
   return *this;
  }
  int get() const noexcept { return fd_; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept {
   if (fd_ >= 0) ::close(fd_);
   fd_ = fd;
  }

 private:
  int fd_;
 };
 struct RunOptions {
  fs::path cwd;
  std::map<std::string, std::string> env;
  std::vector<std::string> unset_env;
  std::optional<std::string> input;
  bool capture_stdout = false;
  bool capture_stderr = false;
  bool merge_stderr = false;
  bool new_process_group = false;
  std::optional<std::chrono::milliseconds> timeout;
  std::function<void(std::string_view)> stdout_sink;
  std::function<void(std::string_view)> stderr_sink;
  // Caller-owned level-triggered event. It stays readable until all command
  // owners settle; run observes it without draining it.
  int cancel_fd = -1;
  std::function<int()> cancel_signal;
 };
 struct ProcessResult {
  int exit_code = 0;
  std::string out;
  std::string err;
  bool timed_out = false;
  int signal = 0;
  int exec_errno = 0;
  bool cancelled = false;
 };
 namespace detail {
 inline FileDescriptor command_descriptor(int fd) {
  FileDescriptor owned(fd);
  if (fd >= 0 && fd <= STDERR_FILENO) {
   const int replacement = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
   if (replacement < 0) throw std::system_error(errno, std::generic_category(), "fcntl command descriptor");
   owned.reset(replacement);
  }
  return owned;
 }
 inline std::pair<FileDescriptor, FileDescriptor> pipe() {
  int descriptors[2];
  if (::pipe2(descriptors, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "pipe2");
  FileDescriptor read_end(descriptors[0]), write_end(descriptors[1]);
  auto safe_read = command_descriptor(read_end.release());
  auto safe_write = command_descriptor(write_end.release());
  return {std::move(safe_read), std::move(safe_write)};
 }
 inline void nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw std::system_error(errno, std::generic_category(), "fcntl");
 }
 inline void spawn_check(int error, const char* operation) {
  if (error) throw std::system_error(error, std::generic_category(), operation);
 }
 template <class Error = ToolError>
 inline std::vector<char*> argument_pointers(const std::vector<std::string>& args) {
  if (args.empty() || args.front().empty()) throw Error("empty command");
  std::vector<char*> result;
  result.reserve(args.size() + 1);
  for (const auto& arg : args) {
   if (arg.find('\0') != std::string::npos) throw Error("NUL byte in command argument");
   result.push_back(const_cast<char*>(arg.c_str()));
  }
  result.push_back(nullptr);
  return result;
 }
 class SpawnActions {
 public:
  posix_spawn_file_actions_t value;
  SpawnActions() { spawn_check(::posix_spawn_file_actions_init(&value), "spawn actions init"); }
  ~SpawnActions() { ::posix_spawn_file_actions_destroy(&value); }
  SpawnActions(const SpawnActions&) = delete;
  SpawnActions& operator=(const SpawnActions&) = delete;
 };
 class SpawnAttributes {
 public:
  posix_spawnattr_t value;
  SpawnAttributes() { spawn_check(::posix_spawnattr_init(&value), "spawn attributes init"); }
  ~SpawnAttributes() { ::posix_spawnattr_destroy(&value); }
  SpawnAttributes(const SpawnAttributes&) = delete;
  SpawnAttributes& operator=(const SpawnAttributes&) = delete;
 };
 }  // namespace detail
 ProcessResult run(const std::vector<std::string>& args, const RunOptions& options = {}) {
  auto pointers = detail::argument_pointers(args);
  const auto cancellation_signal = [&] {
   const int signal = options.cancel_signal ? options.cancel_signal() : SIGTERM;
   if (signal <= 0 || signal >= NSIG) throw ToolError("invalid command cancellation signal");
   return signal;
  };
  if (options.cancel_fd >= 0) {
   pollfd event{options.cancel_fd, POLLIN, 0};
   int ready;
   do { ready = ::poll(&event, 1, 0); } while (ready < 0 && errno == EINTR);
   if (ready < 0) throw std::system_error(errno, std::generic_category(), "poll cancellation");
   if (ready) {
    ProcessResult result;
    result.cancelled = true;
    result.signal = cancellation_signal();
    result.exit_code = 128 + result.signal;
    return result;
   }
  }
  auto output = options.capture_stdout || options.stdout_sink ? detail::pipe() : std::pair<FileDescriptor, FileDescriptor>{};
  auto errors = (options.capture_stderr || options.stderr_sink) && !options.merge_stderr ? detail::pipe() : std::pair<FileDescriptor, FileDescriptor>{};
  // An anonymous file avoids stdin pipe deadlock and SIGPIPE if a command
  // exits without consuming input; output remains streamed through poll.
  FileDescriptor input;
  if (options.input) {
   std::string pattern = (fs::temp_directory_path() / "mmltk-input-XXXXXX").string();
   input.reset(::mkstemp(pattern.data()));
   if (input.get() < 0) throw std::system_error(errno, std::generic_category(), "mkstemp stdin");
   ::unlink(pattern.c_str());
   input = detail::command_descriptor(input.release());
   if (::fcntl(input.get(), F_SETFD, FD_CLOEXEC) < 0) throw std::system_error(errno, std::generic_category(), "fcntl stdin");
   write_all(input.get(), *options.input, "write stdin");
   if (::lseek(input.get(), 0, SEEK_SET) < 0) throw std::system_error(errno, std::generic_category(), "rewind stdin");
  }
  const bool group = options.new_process_group || options.timeout.has_value() || options.cancel_fd >= 0;
  detail::SpawnActions actions;
  detail::SpawnAttributes attributes;
  const auto duplicate = [&](int source, int destination) {
   if (source >= 0) detail::spawn_check(::posix_spawn_file_actions_adddup2(&actions.value, source, destination), "spawn dup2");
  };
  duplicate(input.get(), STDIN_FILENO);
  duplicate(output.second.get(), STDOUT_FILENO);
  duplicate(options.merge_stderr ? STDOUT_FILENO : errors.second.get(), STDERR_FILENO);
  for (const int fd : {input.get(), output.first.get(), output.second.get(), errors.first.get(), errors.second.get()})
   if (fd >= 0) detail::spawn_check(::posix_spawn_file_actions_addclose(&actions.value, fd), "spawn close");
  if (!options.cwd.empty()) detail::spawn_check(::posix_spawn_file_actions_addchdir_np(&actions.value, options.cwd.c_str()), "spawn chdir");
  if (group) {
   detail::spawn_check(::posix_spawnattr_setpgroup(&attributes.value, 0), "spawn process group");
   detail::spawn_check(::posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETPGROUP), "spawn flags");
  }
  // Construct child environment before spawning; never mutate the parent's
  // environment or allocate in a forked copy of a multithreaded process.
  CommandEnvironment environment(options.env, options.unset_env);
  pid_t pid = -1;
  int spawn_error = 0;
  const bool path_changed = options.env.contains("PATH") || std::find(options.unset_env.begin(), options.unset_env.end(), "PATH") != options.unset_env.end();
  if (path_changed && args.front().find('/') == std::string::npos) {
   std::string search_path;
   if (const auto found = options.env.find("PATH"); found != options.env.end())
    search_path = found->second;
   else {
    const auto length = ::confstr(_CS_PATH, nullptr, 0);
    if (length) {
     search_path.resize(length);
     ::confstr(_CS_PATH, search_path.data(), length);
     search_path.pop_back();
    }
   }
   const auto cwd = options.cwd.empty() ? fs::current_path() : fs::absolute(options.cwd);
   spawn_error = search_executable(search_path, args.front(), [&](fs::path candidate) {
    if (candidate.is_relative()) candidate = cwd / candidate;
    return ::posix_spawn(&pid, candidate.c_str(), &actions.value, &attributes.value, pointers.data(), environment.data());
   });
  } else {
   spawn_error = ::posix_spawnp(&pid, pointers.front(), &actions.value, &attributes.value, pointers.data(), environment.data());
  }
  if (spawn_error) {
   ProcessResult result;
   result.exec_errno = spawn_error;
   result.exit_code = spawn_error == ENOENT ? 127 : 126;
   const std::string message = args.front() + ": " + std::strerror(spawn_error) + "\n";
   const bool captured = options.merge_stderr ? options.capture_stdout : options.capture_stderr;
   const auto& sink = options.merge_stderr ? options.stdout_sink : options.stderr_sink;
   if (captured) (options.merge_stderr ? result.out : result.err) = message;
   if (sink) sink(message);
   if (!captured && !sink) {
    const int fd = options.merge_stderr ? STDOUT_FILENO : STDERR_FILENO;
    try_write_all(fd, message);
   }
   return result;
  }
  output.second.reset();
  errors.second.reset();
  input.reset();
  struct ChildGuard {
   pid_t pid;
   bool group;
   bool waited = false;
   ~ChildGuard() {
    if (!waited) {
     ::kill(group ? -pid : pid, SIGKILL);
     while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
   }
  } child{pid, group};
  FileDescriptor child_event;
  if (options.timeout || options.cancel_fd >= 0) {
   child_event.reset(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
   if (child_event.get() < 0) throw std::system_error(errno, std::generic_category(), "pidfd_open");
  }
  ProcessResult result;
  const auto cancel = [&] {
   if (!result.cancelled) {
    result.cancelled = true;
    ::kill(group ? -pid : pid, cancellation_signal());
   }
  };
  const auto deadline = options.timeout ? std::chrono::steady_clock::now() + *options.timeout : std::chrono::steady_clock::time_point::max();
  const auto wait_timeout = [&] {
   return options.timeout && !result.timed_out
           ? static_cast<int>(std::clamp<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count(), 0, INT_MAX))
           : -1;
  };
  for (const int fd : {output.first.get(), errors.first.get()})
   if (fd >= 0) detail::nonblocking(fd);
  auto drain = [](FileDescriptor& fd, std::string& target, bool retain, const std::function<void(std::string_view)>& sink) {
   std::array<char, 16384> buffer{};
   while (fd.get() >= 0) {
    const auto count = ::read(fd.get(), buffer.data(), buffer.size());
    if (count > 0) {
     const std::string_view chunk(buffer.data(), static_cast<std::size_t>(count));
     if (retain) target.append(chunk);
     if (sink) sink(chunk);
     // Return to poll between chunks so a continuous output stream
     // cannot starve the other stream, cancellation, or deadlines.
     return;
    } else if (count == 0)
     fd.reset();
    else if (errno == EINTR)
     continue;
    else if (errno == EAGAIN || errno == EWOULDBLOCK)
     break;
    else
     throw std::system_error(errno, std::generic_category(), "read command output");
   }
  };
  while (output.first.get() >= 0 || errors.first.get() >= 0) {
   const auto now = std::chrono::steady_clock::now();
   if (!result.timed_out && now >= deadline) {
    result.timed_out = true;
    ::kill(group ? -pid : pid, SIGKILL);
   }
   std::array<pollfd, 3> fds{{{output.first.get(), POLLIN, 0}, {errors.first.get(), POLLIN, 0}, {result.cancelled ? -1 : options.cancel_fd, POLLIN, 0}}};
   const int wait_ms = wait_timeout();
   if (::poll(fds.data(), fds.size(), wait_ms) < 0) {
    if (errno == EINTR) continue;
    throw std::system_error(errno, std::generic_category(), "poll command output");
   }
   if (fds[2].revents) cancel();
   if (fds[0].revents) drain(output.first, result.out, options.capture_stdout, options.stdout_sink);
   if (fds[1].revents) drain(errors.first, result.err, options.capture_stderr, options.stderr_sink);
  }
  int status = 0;
  if (child_event.get() >= 0) {
   // Wait on the child's kernel event, including when output is inherited
   // or when a child closes its pipes before it exits.
   while (true) {
    const int wait_ms = wait_timeout();
    std::array<pollfd, 2> events{{{child_event.get(), POLLIN, 0}, {result.cancelled ? -1 : options.cancel_fd, POLLIN, 0}}};
    const int ready = ::poll(events.data(), events.size(), wait_ms);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) throw std::system_error(errno, std::generic_category(), "poll child");
    if (events[1].revents) cancel();
    if (events[0].revents) break;
    if (!result.timed_out && std::chrono::steady_clock::now() >= deadline) {
     result.timed_out = true;
     ::kill(group ? -pid : pid, SIGKILL);
    }
   }
  }
  while (::waitpid(pid, &status, 0) < 0) {
   if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "waitpid");
  }
  child.waited = true;
  if (WIFEXITED(status))
   result.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) {
   result.signal = WTERMSIG(status);
   result.exit_code = 128 + result.signal;
  } else
   result.exit_code = 1;
  return result;
 }
 inline ProcessResult capture(const std::vector<std::string>& args, RunOptions options = {}) {
  options.capture_stdout = true;
  options.capture_stderr = true;
  return run(args, options);
 }
 [[noreturn]] inline void exec(const std::vector<std::string>& args, const RunOptions& options = {}) {
  if (options.input || options.capture_stdout || options.capture_stderr || options.merge_stderr || options.timeout || options.stdout_sink || options.stderr_sink || options.cancel_fd >= 0)
   throw ToolError("exec requires inherited standard streams and no timeout");
  auto pointers = detail::argument_pointers(args);
  if (options.new_process_group && ::setpgid(0, 0) != 0) throw std::system_error(errno, std::generic_category(), "setpgid");
  if (!options.cwd.empty()) fs::current_path(options.cwd);
  for (const auto& name : options.unset_env)
   if (::unsetenv(name.c_str()) != 0) throw std::system_error(errno, std::generic_category(), "unsetenv");
  for (const auto& [name, value] : options.env)
   if (::setenv(name.c_str(), value.c_str(), 1) != 0) throw std::system_error(errno, std::generic_category(), "setenv");
  ::execvp(pointers.front(), pointers.data());
  const int saved_errno = errno;
  throw ToolError(args.front() + ": " + std::strerror(saved_errno), saved_errno == ENOENT ? 127 : 126);
 }
 class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view prefix = "mmltk-") {
   fs::path pattern_path(std::string(prefix) + "XXXXXX");
   if (!pattern_path.is_absolute()) pattern_path = fs::temp_directory_path() / pattern_path;
   std::string pattern = pattern_path.string();
   if (!::mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "mkdtemp");
   path_ = pattern;
  }
  ~TemporaryDirectory() {
   if (!path_.empty()) {
    std::error_code error;
    fs::remove_all(path_, error);
   }
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  const fs::path& path() const noexcept { return path_; }
  fs::path release() noexcept { return std::exchange(path_, {}); }

 private:
  fs::path path_;
 };
 }  // namespace mmltk::tools
}
