module;
// Own a GPU-only Weston session and the packaged Wayland acceptance command.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <initializer_list>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <sys/stat.h>
#include <sched.h>
export module mmltk.tools.headless_compositor;
import mmltk.tools.cpp_linux;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
export {
 namespace mmltk::tools::headless {
 using Command = std::vector<std::string>;
 using Environment = std::map<std::string, std::string>;
 using Renderer = std::map<std::string, std::string>;
 inline constexpr auto STARTUP_TIMEOUT = std::chrono::seconds(30);
 inline constexpr auto PROBE_TIMEOUT = std::chrono::seconds(10);
 inline constexpr auto SHUTDOWN_TIMEOUT = std::chrono::seconds(5);
 class SessionFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
 };
 using mmltk::tools::current_environment;
 Renderer hardware_renderer(std::string_view log);
 void require_wayland_protocols(std::string_view report);
 Environment session_environment(const std::filesystem::path& runtime, Environment environment = current_environment());
 int exit_status(int returncode) noexcept;
 struct SessionOptions {
  Environment environment = current_environment();
  // Empty paths use environment[XDG_RUNTIME_DIR] and the repository's
  // tools/wayland/headless-weston.ini respectively.
  std::filesystem::path runtime_parent;
  std::filesystem::path weston_config;
  // Weston arguments are appended to this executable/argument prefix.
  // The probe command is executed as supplied. All commands use the same
  // process ownership, environment isolation, and hardware admission path.
  Command weston_command{"/usr/bin/weston"};
  Command probe_command{"/usr/bin/wayland-info"};
  std::chrono::milliseconds startup_timeout = STARTUP_TIMEOUT;
  std::chrono::milliseconds probe_timeout = PROBE_TIMEOUT;
  std::chrono::milliseconds shutdown_timeout = SHUTDOWN_TIMEOUT;
 };
 struct ChildStatus {
  pid_t pid = -1;
  // A signal is represented by its negative number, as in subprocess status.
  std::optional<int> returncode;
  bool compositor = false;
 };
 // A session owns its private runtime directory, signal subscription, child
 // processes, and surviving process-group members. run is synchronous and
 // single-use; it always shuts down its children before returning. Construct
 // and run on the same thread, before starting unrelated worker threads, so
 // SIGINT/SIGTERM/SIGHUP are delivered to this owner's signalfd.
 class HeadlessSession {
 public:
  explicit HeadlessSession(std::filesystem::path artifacts, SessionOptions options = {});
  ~HeadlessSession();
  HeadlessSession(const HeadlessSession&) = delete;
  HeadlessSession& operator=(const HeadlessSession&) = delete;
  HeadlessSession(HeadlessSession&&) = delete;
  HeadlessSession& operator=(HeadlessSession&&) = delete;
  int run(const Command& command);
  std::vector<ChildStatus> children() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
 };
 int run_session(const Command& command, const std::filesystem::path& artifacts, SessionOptions options = {});
 int main(const Command& arguments);
 }  // namespace mmltk::tools::headless
}
namespace mmltk::tools::headless {
namespace implementation {
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::milliseconds;
using Deadline = std::optional<Clock::time_point>;
[[noreturn]] void system_failure(std::string_view operation, int error = errno) { throw std::system_error(error, std::generic_category(), std::string(operation)); }
void spawn_check(int error, std::string_view operation) {
 if (error != 0) system_failure(operation, error);
}
FileDescriptor owned_descriptor(int descriptor, std::string_view operation) {
 if (descriptor < 0) system_failure(operation);
 FileDescriptor result(descriptor);
 if (descriptor <= STDERR_FILENO) {
  const int replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (replacement < 0) system_failure("fcntl session descriptor");
  result.reset(replacement);
 }
 return result;
}
FileDescriptor output_file(const fs::path& path) {
 const auto operation = "open " + path.string();
 return owned_descriptor(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666), operation);
}
fs::path unique_directory(const fs::path& parent, std::string_view prefix) {
 std::string pattern = (parent / (std::string(prefix) + "XXXXXX")).string();
 if (::mkdtemp(pattern.data()) == nullptr) system_failure("mkdtemp " + pattern);
 return pattern;
}
fs::path runtime_parent(const SessionOptions& options) {
 if (!options.runtime_parent.empty()) return options.runtime_parent;
 const auto found = options.environment.find("XDG_RUNTIME_DIR");
 if (found == options.environment.end() || found->second.empty()) throw SessionFailure("XDG_RUNTIME_DIR is required");
 return found->second;
}
fs::path weston_config(const SessionOptions& options) {
 if (!options.weston_config.empty()) return options.weston_config;
 const auto found = options.environment.find("MMLTK_REPO_ROOT");
 const auto root = found != options.environment.end() ? fs::path(found->second) : repository_root({}, __FILE__);
 return root / "tools/wayland/headless-weston.ini";
}
class RuntimeDirectory {
public:
 explicit RuntimeDirectory(const fs::path& parent) : path_(unique_directory(parent, "headless-")) {}
 ~RuntimeDirectory() {
  if (path_.empty()) return;
  std::error_code error;
  fs::remove_all(path_, error);
  if (error) std::cerr << "mmltk: headless runtime cleanup failed: " << error.message() << '\n';
 }
 const fs::path& path() const noexcept { return path_; }
 void close() {
  fs::remove_all(path_);
  path_.clear();
 }

private:
 fs::path path_;
};
class SignalSubscription {
public:
 SignalSubscription() {
  sigset_t selected;
  ::sigemptyset(&selected);
  for (const int number : {SIGINT, SIGTERM, SIGHUP}) ::sigaddset(&selected, number);
  if (::sigprocmask(SIG_BLOCK, &selected, &previous_) < 0) system_failure("block session signals");
  try {
   descriptor_ = owned_descriptor(::signalfd(-1, &selected, SFD_CLOEXEC | SFD_NONBLOCK), "signalfd");
  } catch (...) {
   ::sigprocmask(SIG_SETMASK, &previous_, nullptr);
   throw;
  }
 }
 ~SignalSubscription() {
  descriptor_.reset();
  ::sigprocmask(SIG_SETMASK, &previous_, nullptr);
 }
 SignalSubscription(const SignalSubscription&) = delete;
 SignalSubscription& operator=(const SignalSubscription&) = delete;
 int descriptor() const noexcept { return descriptor_.get(); }
 const sigset_t& child_mask() const noexcept { return previous_; }
 int number() const noexcept { return number_; }
 void drain() {
  while (true) {
   signalfd_siginfo information{};
   const auto count = ::read(descriptor_.get(), &information, sizeof(information));
   if (count == static_cast<ssize_t>(sizeof(information))) {
    if (number_ == 0) number_ = static_cast<int>(information.ssi_signo);
   } else if (count < 0 && errno == EINTR) {
    continue;
   } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return;
   } else {
    system_failure("read session signal", count < 0 ? errno : EIO);
   }
  }
 }

private:
 sigset_t previous_{};
 FileDescriptor descriptor_;
 int number_ = 0;
};
int remaining_milliseconds(Deadline deadline) {
 if (!deadline) return -1;
 const auto remaining = *deadline - Clock::now();
 if (remaining <= Clock::duration::zero()) return 0;
 // Round upward, so sub-millisecond intervals never become busy polling.
 const auto count = std::chrono::ceil<Duration>(remaining).count();
 return static_cast<int>(std::min<Duration::rep>(count, INT_MAX));
}
int wait_events(std::vector<pollfd>& events, Deadline deadline) {
 while (true) {
  const int result = ::poll(events.data(), static_cast<nfds_t>(events.size()), remaining_milliseconds(deadline));
  if (result < 0 && errno == EINTR) continue;
  if (result < 0) system_failure("poll session events");
  for (const auto& event : events)
   if (event.revents & POLLNVAL) system_failure("invalid session descriptor", EBADF);
  return result;
 }
}
class Child {
public:
 Child(const Command& command, const Environment& environment, const sigset_t& signal_mask, int output, bool compositor, Duration shutdown_timeout)
     : compositor_(compositor), shutdown_timeout_(shutdown_timeout) {
  auto arguments = detail::argument_pointers<SessionFailure>(command);
  CommandEnvironment<SessionFailure> values(environment, {}, false);
  detail::SpawnActions actions;
  detail::SpawnAttributes attributes;
  if (output >= 0) {
   for (const int destination : {STDOUT_FILENO, STDERR_FILENO}) spawn_check(::posix_spawn_file_actions_adddup2(&actions.value, output, destination), "spawn log redirection");
  }
  // Match subprocess close_fds: the child inherits only its standard
  // streams, including no lifecycle-log or caller-owned descriptors.
  spawn_check(::posix_spawn_file_actions_addclosefrom_np(&actions.value, STDERR_FILENO + 1), "spawn close inherited descriptors");
  sigset_t defaults;
  ::sigemptyset(&defaults);
  for (const int number : {SIGPIPE, SIGINT, SIGTERM, SIGHUP}) ::sigaddset(&defaults, number);
  spawn_check(::posix_spawnattr_setsigdefault(&attributes.value, &defaults), "spawn signal defaults");
  spawn_check(::posix_spawnattr_setsigmask(&attributes.value, &signal_mask), "spawn signal mask");
  spawn_check(::posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF), "spawn session flags");
  const auto spawn = [&](const fs::path& executable) { return ::posix_spawn(&pid_, executable.c_str(), &actions.value, &attributes.value, arguments.data(), values.data()); };
  int error = 0;
  if (command.front().find('/') != std::string::npos) {
   error = spawn(command.front());
  } else {
   const auto path = environment.find("PATH");
   std::string search_path;
   if (path != environment.end()) {
    search_path = path->second;
   } else {
    const auto size = ::confstr(_CS_PATH, nullptr, 0);
    if (size == 0) system_failure("default executable search path");
    search_path.resize(size);
    ::confstr(_CS_PATH, search_path.data(), size);
    search_path.pop_back();
   }
   error = search_executable(search_path, command.front(), spawn);
  }
  if (error != 0) system_failure(command.front(), error);
  try {
   descriptor_ = owned_descriptor(static_cast<int>(::syscall(SYS_pidfd_open, pid_, 0)), "pidfd_open child");
  } catch (...) {
   // No child can escape if descriptor acquisition fails immediately
   // after spawn. SIGKILL is followed by the one mandatory reap.
   ::kill(-pid_, SIGKILL);
   while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
   throw;
  }
 }
 ~Child() {
  if (settled_) return;
  // Exception-only backstop. Ordinary shutdown retains group custody and
  // records escalation before this object is retired.
  ::kill(-pid_, SIGKILL);
  if (!returncode_) {
   pollfd event{descriptor_.get(), POLLIN, 0};
   const auto deadline = Clock::now() + shutdown_timeout_;
   int ready;
   do { ready = ::poll(&event, 1, remaining_milliseconds(deadline)); } while (ready < 0 && errno == EINTR);
   if (ready > 0) {
    int status = 0;
    while (::waitpid(pid_, &status, WNOHANG) < 0 && errno == EINTR) {}
   }
  }
 }
 Child(const Child&) = delete;
 Child& operator=(const Child&) = delete;
 pid_t pid() const noexcept { return pid_; }
 int descriptor() const noexcept { return descriptor_.get(); }
 const std::optional<int>& returncode() const noexcept { return returncode_; }
 bool compositor() const noexcept { return compositor_; }
 bool is_settled() const noexcept { return settled_; }
 void settled() noexcept {
  settled_ = true;
  descriptor_.reset();
 }
 void refresh() {
  if (returncode_) return;
  int status = 0;
  pid_t result;
  do { result = ::waitpid(pid_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
  if (result < 0) system_failure("waitpid child " + std::to_string(pid_));
  if (result == 0) return;
  if (WIFEXITED(status))
   returncode_ = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
   returncode_ = -WTERMSIG(status);
  else
   throw SessionFailure("unrecognized child wait status");
 }

private:
 pid_t pid_ = -1;
 FileDescriptor descriptor_;
 std::optional<int> returncode_;
 bool compositor_ = false;
 bool settled_ = false;
 Duration shutdown_timeout_;
};
std::vector<FileDescriptor> group_pidfds(pid_t group) {
 std::vector<FileDescriptor> descriptors;
 for (const auto& entry : fs::directory_iterator("/proc")) {
  const auto name = entry.path().filename().string();
  pid_t pid = -1;
  const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
  if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size() || pid <= 0) continue;
  const pid_t found = ::getpgid(pid);
  if (found < 0) {
   if (errno == ESRCH || errno == ENOENT) continue;
   system_failure("getpgid " + name);
  }
  if (found != group) continue;
  // A zombie has released resources; its parent or the container init
  // owns reaping. Pin live members even after the group leader exited.
  const int stat_descriptor = ::open((entry.path() / "stat").c_str(), O_RDONLY | O_CLOEXEC);
  if (stat_descriptor < 0) {
   if (errno == ESRCH || errno == ENOENT) continue;
   system_failure("open process stat " + name);
  }
  FileDescriptor stat_file(stat_descriptor);
  std::string stat;
  std::array<char, 4096> buffer{};
  while (true) {
   const auto count = ::read(stat_file.get(), buffer.data(), buffer.size());
   if (count < 0 && errno == EINTR) continue;
   if (count < 0 && (errno == ESRCH || errno == ENOENT)) {
    stat.clear();
    break;
   }
   if (count < 0) system_failure("read process stat " + name);
   if (count == 0) break;
   stat.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (stat.empty()) continue;
  const auto end_name = stat.rfind(')');
  if (end_name == std::string::npos) throw SessionFailure("malformed process stat " + name);
  const auto state = stat.find_first_not_of(" \t\r\n", end_name + 1);
  if (state == std::string::npos) throw SessionFailure("missing process state " + name);
  if (stat[state] == 'Z') continue;
  const int descriptor = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
  if (descriptor < 0) {
   if (errno == ESRCH || errno == ENOENT) continue;
   system_failure("pidfd_open process group member " + name);
  }
  FileDescriptor pinned = owned_descriptor(descriptor, "pidfd_open process group member");
  const pid_t pinned_group = ::getpgid(pid);
  if (pinned_group < 0 && errno != ESRCH && errno != ENOENT) system_failure("getpgid pinned member " + name);
  if (pinned_group == group) descriptors.push_back(std::move(pinned));
 }
 return descriptors;
}
void signal_group(pid_t group, int number) {
 if (::kill(-group, number) < 0 && errno != ESRCH) system_failure("signal process group");
}
std::string timeout_message(Duration timeout) {
 std::ostringstream message;
 message << "operation exceeded " << std::chrono::duration<double>(timeout).count() << "s deadline";
 return message.str();
}
bool ready_message(std::string_view message) {
 for (std::size_t start = 0; start < message.size();) {
  const auto end = message.find_first_of("\r\n", start);
  if (message.substr(start, end == std::string_view::npos ? end : end - start) == "READY=1") return true;
  if (end == std::string_view::npos) break;
  start = end + 1;
  if (message[end] == '\r' && start < message.size() && message[start] == '\n') ++start;
 }
 return false;
}
}  // namespace implementation
using namespace implementation;
Renderer hardware_renderer(std::string_view log) {
 Renderer fields;
 for (const std::string name : {"EGL vendor", "GL vendor", "GL renderer"}) {
  const std::regex pattern("\\b" + name + R"(:\s*([^\n]+))");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(log.begin(), log.end(), match, pattern)) throw SessionFailure("Weston did not report " + name);
  fields[name] = trim(match[1].str());
 }
 const auto render_fields = [&] { return json(fields).dump(-1, ' ', false, json::error_handler_t::replace); };
 for (const auto* name : {"EGL vendor", "GL vendor"})
  if (fields.at(name).find("NVIDIA") == std::string::npos) throw SessionFailure("Weston requires NVIDIA EGL and GL: " + render_fields());
 if (std::regex_search(fields.at("GL renderer"), std::regex("llvmpipe|softpipe|swrast|software", std::regex::icase))) throw SessionFailure("Weston selected a software renderer: " + render_fields());
 const std::regex modifiers(R"(\bdmabuf support:\s*modifiers\s*$)", std::regex::ECMAScript | std::regex::multiline);
 if (!std::regex_search(log.begin(), log.end(), modifiers)) throw SessionFailure("NVIDIA EGL does not expose DMA-BUF import modifiers");
 return fields;
}
void require_wayland_protocols(std::string_view report) {
 for (const std::string name : {"wl_compositor", "wl_output", "xdg_wm_base", "zwp_linux_dmabuf_v1", "wl_seat"}) {
  const std::regex pattern("interface:\\s*'" + name + "'");
  if (!std::regex_search(report.begin(), report.end(), pattern)) throw SessionFailure("headless compositor lacks " + name);
 }
 bool seat = false;
 bool inspected_capabilities = false;
 const std::regex capabilities(R"(^\s+capabilities:([^\n]*)$)");
 for (const auto& line : lines(report)) {
  if (line.starts_with("interface:")) {
   seat = line.starts_with("interface: 'wl_seat'");
   inspected_capabilities = false;
  }
  if (!seat || inspected_capabilities) continue;
  std::smatch match;
  if (!std::regex_search(line, match, capabilities)) continue;
  inspected_capabilities = true;
  std::istringstream words(match[1].str());
  bool pointer = false;
  bool keyboard = false;
  for (std::string word; words >> word;) {
   pointer = pointer || word == "pointer";
   keyboard = keyboard || word == "keyboard";
  }
  if (pointer && keyboard) return;
 }
 throw SessionFailure("headless compositor lacks a wl_seat with pointer and keyboard");
}
Environment session_environment(const fs::path& runtime, Environment environment) {
 for (const auto* name : {
       "DISPLAY",
       "WAYLAND_DISPLAY",
       "WAYLAND_SOCKET",
       "WAYLAND_SERVER_SOCKET",
       "DBUS_SESSION_BUS_ADDRESS",
       "MOZ_HEADLESS",
       "LIBGL_ALWAYS_SOFTWARE",
       "GALLIUM_DRIVER",
       "NOTIFY_SOCKET",
       "WATCHDOG_USEC",
       "WATCHDOG_PID",
       "LISTEN_FDS",
       "LISTEN_PID",
       "LISTEN_FDNAMES",
      })
  environment.erase(name);
 environment["XDG_RUNTIME_DIR"] = runtime.string();
 environment["XDG_SESSION_TYPE"] = "wayland";
 environment["WAYLAND_DISPLAY"] = "wayland-mmltk";
 environment["GDK_BACKEND"] = "wayland";
 environment["MOZ_ENABLE_WAYLAND"] = "1";
 environment["GBM_BACKEND"] = "nvidia-drm";
 environment["__EGL_VENDOR_LIBRARY_FILENAMES"] = "/usr/share/glvnd/egl_vendor.d/10_nvidia.json";
 return environment;
}
int exit_status(int returncode) noexcept { return returncode >= 0 ? returncode : 128 - returncode; }
class HeadlessSession::Impl {
public:
 Impl(fs::path artifacts, SessionOptions options) : artifacts_(std::move(artifacts)), options_(std::move(options)) {}
 int run(const Command& command) {
  if (started_) throw SessionFailure("headless session has already run");
  started_ = true;
  if (options_.startup_timeout < Duration::zero() || options_.probe_timeout < Duration::zero() || options_.shutdown_timeout < Duration::zero())
   throw SessionFailure("session timeouts must be nonnegative");
  // These scoped owners outlive all children and shutdown work. Keeping
  // them local restores signal delivery and removes runtime files when
  // run returns, including when the public session remains inspectable.
  RuntimeDirectory runtime(runtime_parent(options_));
  SignalSubscription signals;
  signals_ = &signals;
  environment_ = session_environment(runtime.path(), options_.environment);
  events_.open(artifacts_ / "supervisor.jsonl", std::ios::out | std::ios::trunc);
  if (!events_) {
   signals_ = nullptr;
   throw SessionFailure("cannot write " + (artifacts_ / "supervisor.jsonl").string());
  }
  int code = 1;
  try {
   code = execute(command, runtime.path());
  } catch (const std::exception& error) { failure(error.what()); }
  if (!close() && code == 0) {
   std::cerr << "mmltk: headless compositor failed: unclean session shutdown; artifacts: " << artifacts_.string() << '\n';
   code = 1;
  }
  try {
   runtime.close();
  } catch (const std::exception& error) {
   failure(error.what());
   if (code == 0) code = 1;
  }
  try {
   signals.drain();
   if (signals.number() != 0) code = 128 + signals.number();
   event("finished", {{"exit_code", code}});
  } catch (const std::exception& error) {
   std::cerr << "mmltk: headless compositor failed: " << error.what() << "; artifacts: " << artifacts_.string() << '\n';
   if (code == 0) code = 1;
  }
  events_.close();
  if (events_.fail()) {
   std::cerr << "mmltk: headless compositor failed: cannot close supervisor lifecycle log; artifacts: " << artifacts_.string() << '\n';
   if (code == 0) code = 1;
  }
  signals_ = nullptr;
  return code;
 }
 std::vector<ChildStatus> children() const {
  std::vector<ChildStatus> result;
  result.reserve(children_.size());
  for (const auto& child : children_) result.push_back({child->pid(), child->returncode(), child->compositor()});
  return result;
 }

private:
 void event(std::string_view name, json fields = json::object()) {
  json record = {
   {"event", "headless." + std::string(name)},
   {"steady_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()},
   {"pid", ::getpid()},
  };
  record.update(fields);
  events_ << record.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
  events_.flush();
  if (!events_) throw SessionFailure("cannot write supervisor lifecycle log");
 }
 void failure(std::string_view message) noexcept {
  try {
   event("failed", {{"message", message}});
  } catch (const std::exception&) {}
  std::cerr << "mmltk: headless compositor failed: " << message << "; artifacts: " << artifacts_.string() << '\n';
 }
 void cleanup_event(std::string_view name, pid_t pid, std::string_view message = {}) noexcept {
  try {
   json fields = {{"child_pid", pid}};
   if (!message.empty()) fields["message"] = message;
   event(name, std::move(fields));
  } catch (const std::exception& error) { std::cerr << "mmltk: headless compositor lifecycle logging failed: " << error.what() << '\n'; }
 }
 Child& start(const Command& command, const Environment& environment, int output = -1, bool compositor = false) {
  auto child = std::make_unique<Child>(command, environment, signals_->child_mask(), output, compositor, options_.shutdown_timeout);
  // Allocation failure while adding the owner still kills and reaps the
  // locally owned child through its destructor.
  children_.push_back(std::move(child));
  return *children_.back();
 }
 void guard() {
  signals_->drain();
  if (signals_->number() != 0) throw SessionFailure("session interrupted by signal " + std::to_string(signals_->number()));
  compositor_->refresh();
  if (compositor_->returncode()) throw SessionFailure("Weston exited unexpectedly with status " + std::to_string(*compositor_->returncode()));
 }
 void await_ready(int notification) {
  const auto deadline = Clock::now() + options_.startup_timeout;
  std::vector<pollfd> events{{signals_->descriptor(), POLLIN, 0}, {compositor_->descriptor(), POLLIN, 0}, {notification, POLLIN, 0}};
  std::array<char, 4096> message{};
  while (true) {
   guard();
   if (wait_events(events, deadline) == 0) throw SessionFailure(timeout_message(options_.startup_timeout));
   guard();
   if (!(events[2].revents & POLLIN)) continue;
   const auto count = ::recv(notification, message.data(), message.size(), 0);
   if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
   if (count < 0) system_failure("receive Weston readiness");
   if (ready_message(std::string_view(message.data(), static_cast<std::size_t>(count)))) return;
   if (Clock::now() >= deadline) throw SessionFailure(timeout_message(options_.startup_timeout));
  }
 }
 int await_child(Child& child, std::optional<Duration> timeout = std::nullopt) {
  const Deadline deadline = timeout ? Deadline(Clock::now() + *timeout) : std::nullopt;
  std::vector<pollfd> events{{signals_->descriptor(), POLLIN, 0}, {compositor_->descriptor(), POLLIN, 0}, {child.descriptor(), POLLIN, 0}};
  while (true) {
   guard();
   child.refresh();
   if (child.returncode()) return *child.returncode();
   if (wait_events(events, deadline) == 0) throw SessionFailure(timeout_message(*timeout));
  }
 }
 int execute(const Command& command, const fs::path& runtime) {
  auto notification = owned_descriptor(::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), "socket Weston readiness");
  const auto notification_path = (runtime / "notify").string();
  const UnixAddress<SessionFailure> address(notification_path, "Weston readiness socket path is too long");
  if (::bind(notification.get(), address.get(), address.length) < 0) system_failure("bind Weston readiness");
  auto compositor_environment = environment_;
  compositor_environment["NOTIFY_SOCKET"] = notification_path;
  Command weston = options_.weston_command;
  if (weston.empty()) throw SessionFailure("empty Weston command");
  for (const auto* argument : {
        "--backend=headless",
        "--renderer=gl",
        "--width=1920",
        "--height=1080",
        "--scale=1",
        "--idle-time=0",
        "--shell=desktop",
        "--modules=/usr/lib/weston/mmltk-headless-seat.so,systemd-notify.so",
       })
   weston.emplace_back(argument);
  const auto log = artifacts_ / "weston.log";
  weston.push_back("--config=" + weston_config(options_).string());
  weston.push_back("--socket=" + environment_.at("WAYLAND_DISPLAY"));
  weston.push_back("--log=" + log.string());
  event("starting", {{"command", weston}, {"test_command", command}});
  auto errors = output_file(artifacts_ / "weston-stderr.log");
  compositor_ = &start(weston, compositor_environment, errors.get(), true);
  await_ready(notification.get());
  const auto renderer = hardware_renderer(read_text(log));
  auto report = output_file(artifacts_ / "wayland-info.log");
  auto& probe = start(options_.probe_command, environment_, report.get());
  const int probe_code = await_child(probe, options_.probe_timeout);
  if (probe_code != 0) throw SessionFailure("wayland-info exited with status " + std::to_string(probe_code));
  require_wayland_protocols(read_text(artifacts_ / "wayland-info.log"));
  // Match the successful probe's retirement before command execution.
  // Its status remains inspectable, but its numeric group must not be
  // signaled again after an arbitrarily long acceptance command.
  probe.settled();
  event("ready", {{"compositor_pid", compositor_->pid()}, {"renderer", renderer}});
  std::cerr << "mmltk: headless NVIDIA compositor ready; artifacts: " << artifacts_.string() << '\n';
  guard();
  auto& process = start(command, environment_);
  event("test.started", {{"child_pid", process.pid()}});
  const int code = await_child(process);
  event("test.exited", {{"child_pid", process.pid()}, {"returncode", code}});
  return exit_status(code);
 }
 bool wait_group(Child& process, const std::vector<FileDescriptor>& descriptors) {
  const auto deadline = Clock::now() + options_.shutdown_timeout;
  std::vector<pollfd> events;
  events.reserve(descriptors.size() + 2);
  events.push_back({signals_->descriptor(), POLLIN, 0});
  events.push_back({process.returncode() ? -1 : process.descriptor(), POLLIN, 0});
  for (const auto& descriptor : descriptors) events.push_back({descriptor.get(), POLLIN, 0});
  while (true) {
   process.refresh();
   if (process.returncode()) events[1].fd = -1;
   const bool completed = std::all_of(events.begin() + 1, events.end(), [](const pollfd& event) { return event.fd < 0; });
   if (completed) return true;
   if (wait_events(events, deadline) == 0) return false;
   if (events[0].revents) signals_->drain();
   for (std::size_t index = 1; index < events.size(); ++index)
    if (events[index].revents & (POLLIN | POLLHUP)) events[index].fd = -1;
   if (Clock::now() >= deadline) {
    process.refresh();
    return std::all_of(events.begin() + 1, events.end(), [](const pollfd& event) { return event.fd < 0; }) && process.returncode().has_value();
   }
  }
 }
 bool stop_group(Child& process) {
  bool clean = true;
  for (const int number : {SIGTERM, SIGKILL}) {
   auto descriptors = group_pidfds(process.pid());
   if (number == SIGKILL && descriptors.empty()) break;
   if (number == SIGKILL) {
    clean = false;
    cleanup_event("cleanup.escalated", process.pid());
   }
   signal_group(process.pid(), number);
   if (!wait_group(process, descriptors)) {
    clean = false;
    if (number == SIGKILL) {
     constexpr auto message = "process group did not exit after SIGKILL";
     cleanup_event("cleanup.failed", process.pid(), message);
     std::cerr << "mmltk: headless compositor cleanup failed: " << message << " (group " << process.pid() << ")\n";
    }
   }
  }
  process.refresh();
  if (process.returncode()) process.settled();
  return clean;
 }
 bool close() noexcept {
  bool clean = true;
  for (auto iterator = children_.rbegin(); iterator != children_.rend(); ++iterator) {
   auto& process = **iterator;
   if (process.is_settled()) continue;
   bool was_running = !process.returncode();
   try {
    process.refresh();
    was_running = !process.returncode();
    clean = stop_group(process) && clean;
   } catch (const std::exception& error) {
    clean = false;
    cleanup_event("cleanup.failed", process.pid(), error.what());
    std::cerr << "mmltk: headless compositor cleanup failed: " << error.what() << " (group " << process.pid() << ")\n";
    cleanup_event("cleanup.escalated", process.pid());
    // A failed procfs inspection must not strand other owners.
    // Continue to the next group even if this fallback also fails.
    try {
     signal_group(process.pid(), SIGKILL);
     if (wait_group(process, {})) process.settled();
    } catch (const std::exception& fallback) { std::cerr << "mmltk: headless compositor cleanup failed: " << fallback.what() << '\n'; }
   }
   if (process.compositor()) {
    try {
     const json code = process.returncode() ? json(*process.returncode()) : json(nullptr);
     event("compositor.exited", {{"returncode", code}, {"shutdown_requested", was_running}});
    } catch (const std::exception& error) {
     clean = false;
     std::cerr << "mmltk: headless compositor lifecycle logging failed: " << error.what() << '\n';
    }
    if (!was_running || process.returncode() != std::optional<int>(0)) clean = false;
   }
  }
  return clean;
 }
 fs::path artifacts_;
 SessionOptions options_;
 Environment environment_;
 std::ofstream events_;
 std::vector<std::unique_ptr<Child>> children_;
 Child* compositor_ = nullptr;
 SignalSubscription* signals_ = nullptr;
 bool started_ = false;
};
HeadlessSession::HeadlessSession(fs::path artifacts, SessionOptions options) : impl_(std::make_unique<Impl>(std::move(artifacts), std::move(options))) {}
HeadlessSession::~HeadlessSession() = default;
int HeadlessSession::run(const Command& command) { return impl_->run(command); }
std::vector<ChildStatus> HeadlessSession::children() const { return impl_->children(); }
int run_session(const Command& command, const fs::path& artifacts, SessionOptions options) {
 try {
  HeadlessSession session(artifacts, std::move(options));
  return session.run(command);
 } catch (const std::exception& error) {
  std::cerr << "mmltk: headless compositor failed: " << error.what() << "; artifacts: " << artifacts.string() << '\n';
  return 1;
 }
}
int main(const Command& arguments) {
 Command command = arguments;
 if (!command.empty() && (command.front() == "-h" || command.front() == "--help")) {
  std::cout << "usage: headless_compositor [-h] ...\n\n"
               "Own a GPU-only Weston session and the packaged Wayland acceptance command.\n\n"
               "positional arguments:\n  command\n\noptions:\n  -h, --help  show this help message and exit\n";
  return 0;
 }
 if (!command.empty() && command.front() == "--")
  command.erase(command.begin());
 else if (!command.empty() && command.front().starts_with('-')) {
  std::cerr << "usage: headless_compositor [-h] ...\nheadless_compositor: error: unrecognized arguments: " << command.front() << '\n';
  return 2;
 }
 if (command.empty()) {
  std::cerr << "usage: headless_compositor [-h] ...\nheadless_compositor: error: a command is required\n";
  return 2;
 }
 try {
  const auto root = optional_env("MMLTK_REPO_ROOT");
  if (!root) throw SessionFailure("MMLTK_REPO_ROOT is required");
  const fs::path validation = fs::path(*root) / "build/validation";
  fs::create_directories(validation);
  const auto artifacts = unique_directory(validation, "headless-compositor-" + std::to_string(::getpid()) + '-');
  return run_session(command, artifacts);
 } catch (const std::exception& error) {
  std::cerr << "mmltk: headless compositor failed: " << error.what() << '\n';
  return 1;
 }
}
}  // namespace mmltk::tools::headless
export int mmltk_tool_headless_compositor(int argc, char** argv) { return mmltk::tools::headless::main({argv + 1, argv + argc}); }
