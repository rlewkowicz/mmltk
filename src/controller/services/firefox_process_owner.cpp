#include "src/controller/services/firefox_process_owner.h"
#include "src/common/io/event_fd.h"
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>
#include "src/common/io/file_memory.h"
#include "src/common/io/scoped_fd.h"
namespace mmltk::controller::services {
namespace {
using mmltk::common::io::ScopedFd;
constexpr auto kMaximumFirefoxStopGrace = std::chrono::milliseconds{60'000};
constexpr std::uint32_t kInitialFirefoxWindowWidth = 1'500U;
constexpr std::uint32_t kInitialFirefoxWindowHeight = 1'125U;
[[nodiscard]] bool environment_entry_has_key(const std::string_view entry, const std::string_view key) noexcept {
 return entry.size() > key.size() && entry.starts_with(key) && entry[key.size()] == '=';
}
[[nodiscard]] std::vector<std::string> firefox_environment(const std::filesystem::path& runtime_root, const std::filesystem::path& workspace_import_socket,
                                                           const bool integration, const bool integration_high_dpi) {
 static constexpr std::array<std::string_view, 12U> kOverridden{
  "DISPLAY",           "GDK_BACKEND",        "LD_LIBRARY_PATH", "MOZILLA_FIVE_HOME", "MOZ_CRASHREPORTER_DISABLE", "MOZ_DBUS_REMOTE",
  "MOZ_DEFAULT_PREFS", "MOZ_ENABLE_WAYLAND", "MOZ_NOREMOTE",    "NO_AT_BRIDGE",      "XDG_SESSION_TYPE",          "MMLTK_WORKSPACE_IMPORT_SOCKET"};
 std::vector<std::string> result;
 std::string inherited_library_path;
 for (char** current = environ; current && *current; ++current) {
  const std::string_view entry{*current};
  if (environment_entry_has_key(entry, "LD_LIBRARY_PATH")) inherited_library_path.assign(entry.substr(16U));
  if (!std::ranges::any_of(kOverridden, [&](const auto key) { return environment_entry_has_key(entry, key); })) result.emplace_back(entry);
 }
 result.emplace_back("GDK_BACKEND=wayland");
 result.emplace_back("MOZILLA_FIVE_HOME=" + runtime_root.string());
 result.emplace_back("MOZ_CRASHREPORTER_DISABLE=1");
 result.emplace_back("MOZ_ENABLE_WAYLAND=1");
 result.emplace_back("MOZ_NOREMOTE=1");
 result.emplace_back("NO_AT_BRIDGE=1");
 result.emplace_back("XDG_SESSION_TYPE=wayland");
 result.emplace_back("MMLTK_WORKSPACE_IMPORT_SOCKET=" + workspace_import_socket.string());
 std::string default_preferences =
  "pref(\"toolkit.shutdown.fastShutdownStage\", 0);\n"
  "pref(\"dom.allow_scripts_to_close_windows\", true);\n"
  "pref(\"dom.ipc.processPrelaunch.enabled\", false);\n";
 if (integration) {
  default_preferences +=
   "pref(\"full-screen-api.allow-trusted-requests-only\", false);\n"
   "pref(\"permissions.fullscreen.allowed\", true);\n"
   "pref(\"full-screen-api.warning.timeout\", 0);\n";
  default_preferences += integration_high_dpi ? "pref(\"layout.css.devPixelsPerPx\", \"1.5\");\n" : "pref(\"layout.css.devPixelsPerPx\", \"1\");\n";
 }
 result.emplace_back("MOZ_DEFAULT_PREFS=" + std::move(default_preferences));
 std::string library_path = "LD_LIBRARY_PATH=" + runtime_root.string();
 if (!inherited_library_path.empty()) library_path += ":" + inherited_library_path;
 result.push_back(std::move(library_path));
 return result;
}
[[nodiscard]] sigset_t runtime_signal_set() noexcept {
 sigset_t signals{};
 if (::sigemptyset(&signals) != 0 || ::sigaddset(&signals, SIGINT) != 0 || ::sigaddset(&signals, SIGTERM) != 0) std::terminate();
 return signals;
}
[[nodiscard]] bool runtime_signals_are_blocked() noexcept {
 sigset_t current{};
 return ::pthread_sigmask(SIG_SETMASK, nullptr, &current) == 0 && ::sigismember(&current, SIGINT) == 1 && ::sigismember(&current, SIGTERM) == 1;
}
[[nodiscard]] int child_runtime_signal_mask(sigset_t& signals) noexcept {
 const int error = ::pthread_sigmask(SIG_SETMASK, nullptr, &signals);
 if (error != 0) return error;
 return ::sigdelset(&signals, SIGINT) == 0 && ::sigdelset(&signals, SIGTERM) == 0 ? 0 : errno;
}
[[nodiscard]] bool signal_child(const int descriptor, const pid_t pid, const int signal) noexcept {
 if (signal == 0) return true;
 if (descriptor >= 0) {
  int result = -1;
  do { result = static_cast<int>(::syscall(SYS_pidfd_send_signal, descriptor, signal, nullptr, 0U)); } while (result != 0 && errno == EINTR);
  if (result == 0 || errno == ESRCH) return true;
 }
 int result = -1;
 do { result = ::kill(pid, signal); } while (result != 0 && errno == EINTR);
 return result == 0 || errno == ESRCH;
}
struct ChildSettlement final {
 siginfo_t child{};
};
class ChildCustody final {
public:
 ChildCustody() = default;
 ~ChildCustody() noexcept {
  if (installed()) std::terminate();
 }
 ChildCustody(const ChildCustody&) = delete;
 ChildCustody& operator=(const ChildCustody&) = delete;
 void Install(const pid_t pid) noexcept {
  if (pid <= 0 || pid_ > 0) std::terminate();
  pid_ = pid;
 }
 void InstallPidfd(ScopedFd pidfd) noexcept {
  if (!installed()) std::terminate();
  pidfd_ = std::move(pidfd);
 }
 [[nodiscard]] pid_t pid() const noexcept { return pid_; }
 [[nodiscard]] int poll_fd() const noexcept { return pidfd_.get(); }
 [[nodiscard]] bool installed() const noexcept { return pid_ > 0; }
 [[nodiscard]] bool Signal(const int signal) const noexcept { return signal_child(pidfd_.get(), pid_, signal); }
 void PreferRetainedPidWaitForTest() noexcept { prefer_retained_pid_wait_ = true; }
 [[nodiscard]] ChildSettlement Settle(const RuntimeDiagnosticTarget diagnostics) noexcept {
  if (pid_ <= 0) std::terminate();
  siginfo_t child{};
  if (pidfd_.get() >= 0 && !prefer_retained_pid_wait_) {
   for (;;) {
    child = {};
    errno = 0;
    if (::waitid(P_PIDFD, static_cast<id_t>(pidfd_.get()), &child, WEXITED) == 0 && child.si_pid != 0) return Complete(child);
    if (errno == EINTR) continue;
    if (errno == ECHILD) return Complete({});
    break;
   }
  }
  for (;;) {
   child = {};
   errno = 0;
   if (::waitid(P_PID, static_cast<id_t>(pid_), &child, WEXITED) == 0 && child.si_pid != 0) return Complete(child);
   if (errno == EINTR) continue;
   if (errno == ECHILD) return Complete({});
   break;
  }
  int status = 0;
  pid_t waited = -1;
  do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
  if (waited == pid_) {
   child = {};
   child.si_pid = pid_;
   if (WIFEXITED(status)) {
    child.si_code = CLD_EXITED;
    child.si_status = WEXITSTATUS(status);
   } else {
    child.si_code = CLD_KILLED;
    child.si_status = WIFSIGNALED(status) ? WTERMSIG(status) : SIGKILL;
   }
   return Complete(child);
  }
  if (waited < 0 && errno == ECHILD) return Complete({});
  if (diagnostics.valid())
   diagnostics.Emit([&] {
    return RuntimeDiagnosticFact{
     .owner = contracts::DiagnosticOwner::FirefoxProcess,
     .event = "child.custody_invariant",
     .sequence = static_cast<std::uint64_t>(pid_),
     .value = static_cast<std::uint64_t>(static_cast<std::uint32_t>(waited < 0 ? errno : EIO)),
    };
   });
  std::terminate();
 }

private:
 [[nodiscard]] ChildSettlement Complete(const siginfo_t child) noexcept {
  pid_ = -1;
  pidfd_.reset();
  return {.child = child};
 }
 pid_t pid_ = -1;
 ScopedFd pidfd_;
 bool prefer_retained_pid_wait_ = false;
};
class SpawnFileActions final {
public:
 ~SpawnFileActions() noexcept {
  if (initialized_) static_cast<void>(::posix_spawn_file_actions_destroy(&actions_));
 }
 SpawnFileActions() = default;
 SpawnFileActions(const SpawnFileActions&) = delete;
 SpawnFileActions& operator=(const SpawnFileActions&) = delete;
 [[nodiscard]] bool initialize() noexcept {
  const int error = initialized_ ? EINVAL : ::posix_spawn_file_actions_init(&actions_);
  initialized_ = error == 0;
  if (error != 0) errno = error;
  return initialized_;
 }
 [[nodiscard]] bool add_dup2(int source, int destination) noexcept {
  const int error = initialized_ ? ::posix_spawn_file_actions_adddup2(&actions_, source, destination) : EINVAL;
  if (error != 0) errno = error;
  return error == 0;
 }
 [[nodiscard]] bool add_close(int descriptor) noexcept {
  const int error = initialized_ ? ::posix_spawn_file_actions_addclose(&actions_, descriptor) : EINVAL;
  if (error != 0) errno = error;
  return error == 0;
 }
 [[nodiscard]] const posix_spawn_file_actions_t* get() const noexcept { return initialized_ ? &actions_ : nullptr; }

private:
 posix_spawn_file_actions_t actions_{};
 bool initialized_ = false;
};
}  // namespace
class FirefoxProcessOwner::Implementation final {
public:
 Implementation(std::filesystem::path workspace_import_socket, FirefoxProcessConfig config, FirefoxProcessObservationTarget observations,
                RuntimeDiagnosticTarget diagnostics) noexcept
     : workspace_import_socket_(std::move(workspace_import_socket)),
       executable_(std::move(config.executable)),
       page_url_(std::move(config.page_url)),
       log_file_(std::move(config.log_file)),
       observations_(observations),
       diagnostics_(diagnostics),
       stop_grace_(config.stop_grace),
       integration_(config.integration),
       integration_high_dpi_(config.integration_high_dpi) {}
 ~Implementation() noexcept {
  request_stop();
  wait();
  cleanup_profile();
 }
 [[nodiscard]] FirefoxProcessStartResult start() noexcept {
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled()) return FirefoxProcessStartResult::Terminal;
   if (custody_.installed()) return monitor_.joinable() ? FirefoxProcessStartResult::ChildInstalled : FirefoxProcessStartResult::Terminal;
  }
  if (!valid_launch_inputs() || !prepare_profile()) return startup_failed("start.invalid");
  std::array<std::string, 10U> argument_storage;
  std::vector<std::string> environment_storage;
  std::vector<char*> environment;
  try {
   argument_storage = {executable_.string(),
                       "--no-remote",
                       "--new-instance",
                       "--profile",
                       profile_.string(),
                       "--width",
                       std::to_string(kInitialFirefoxWindowWidth),
                       "--height",
                       std::to_string(kInitialFirefoxWindowHeight),
                       page_url_};
   environment_storage = firefox_environment(executable_.parent_path(), workspace_import_socket_, integration_, integration_high_dpi_);
   environment.reserve(environment_storage.size() + 1U);
   for (auto& entry : environment_storage) environment.push_back(entry.data());
   environment.push_back(nullptr);
  } catch (...) { return startup_failed("start.arguments_refused"); }
  std::array<char*, 11U> arguments{argument_storage[0].data(),
                                   argument_storage[1].data(),
                                   argument_storage[2].data(),
                                   argument_storage[3].data(),
                                   argument_storage[4].data(),
                                   argument_storage[5].data(),
                                   argument_storage[6].data(),
                                   argument_storage[7].data(),
                                   argument_storage[8].data(),
                                   argument_storage[9].data(),
                                   nullptr};
  SpawnFileActions file_actions;
  if (!prepare_log_handoff(file_actions)) return startup_failed("start.log_handoff_refused", errno);
  posix_spawnattr_t attributes{};
  const int attributes_error = ::posix_spawnattr_init(&attributes);
  if (attributes_error != 0) return startup_failed("start.spawn_attributes_refused", attributes_error);
  sigset_t child_mask{};
  int signal_error = child_runtime_signal_mask(child_mask);
  if (signal_error == 0) signal_error = ::posix_spawnattr_setsigmask(&attributes, &child_mask);
  if (signal_error == 0) signal_error = ::posix_spawnattr_setpgroup(&attributes, 0);
  if (signal_error == 0) signal_error = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);
  if (signal_error != 0) {
   static_cast<void>(::posix_spawnattr_destroy(&attributes));
   return startup_failed("start.signal_mask_refused", signal_error);
  }
  pid_t child = -1;
  const int spawn_result = ::posix_spawn(&child, executable_.c_str(), file_actions.get(), &attributes, arguments.data(), environment.data());
  const int destroy_result = ::posix_spawnattr_destroy(&attributes);
  firefox_log_.reset();
  if (spawn_result != 0) return startup_failed("start.spawn_refused", spawn_result);
  {
   std::scoped_lock lock{mutex_};
   custody_.Install(child);
  }
  if (destroy_result != 0) {
   settle_failure("start.spawn_attributes_destroy_refused", destroy_result, destroy_result);
   return FirefoxProcessStartResult::Terminal;
  }
  ScopedFd pidfd{static_cast<int>(::syscall(SYS_pidfd_open, child, 0U))};
  int setup_error = pidfd.get() < 0 ? errno : 0;
  ScopedFd stop_fd{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)};
  if (stop_fd.get() < 0 && setup_error == 0) setup_error = errno;
  ScopedFd timer_fd{::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)};
  if (timer_fd.get() < 0 && setup_error == 0) setup_error = errno;
  {
   std::scoped_lock lock{mutex_};
   custody_.InstallPidfd(std::move(pidfd));
   stop_fd_ = std::move(stop_fd);
   timer_fd_ = std::move(timer_fd);
  }
  if (custody_.poll_fd() < 0 || stop_fd_.get() < 0 || timer_fd_.get() < 0 || !observations_.install(child)) {
   settle_failure("child.integration_refused", errno, setup_error);
   return FirefoxProcessStartResult::Terminal;
  }
  trace("child.spawned");
  try {
   monitor_ = std::jthread([this] { monitor(); });
  } catch (const std::system_error& error) {
   settle_failure("child.monitor_refused", EAGAIN, error.code().value());
   return FirefoxProcessStartResult::Terminal;
  } catch (...) {
   settle_failure("child.monitor_refused", EAGAIN);
   return FirefoxProcessStartResult::Terminal;
  }
  return FirefoxProcessStartResult::ChildInstalled;
 }
 void request_stop() noexcept {
  int error = 0;
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled() || !custody_.installed()) return;
   lifecycle_.stop_requested = true;
   const std::uint64_t value = 1U;
   ssize_t written = -1;
   do { written = ::write(stop_fd_.get(), &value, sizeof(value)); } while (written < 0 && errno == EINTR);
   if (written == static_cast<ssize_t>(sizeof(value)) || (written < 0 && errno == EAGAIN)) return;
   error = written < 0 ? errno : EIO;
   record_infrastructure_failure_locked(error, written < 0 ? error : 0);
   static_cast<void>(custody_.Signal(SIGKILL));
   const itimerspec immediate{
    .it_interval = {},
    .it_value = {.tv_sec = 0, .tv_nsec = 1L},
   };
   static_cast<void>(::timerfd_settime(timer_fd_.get(), 0, &immediate, nullptr));
  }
  trace("stop.wake_refused", error);
 }
 void wait() noexcept {
  if (monitor_.joinable()) monitor_.join();
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled() || !custody_.installed()) return;
  }
  settle_failure("child.wait_settlement", EIO);
 }
 [[nodiscard]] FirefoxProcessLifecycle lifecycle() const noexcept {
  std::scoped_lock lock{mutex_};
  return lifecycle_;
 }
 void prefer_retained_pid_wait_for_test() noexcept {
  std::scoped_lock lock{mutex_};
  custody_.PreferRetainedPidWaitForTest();
 }

private:
 void monitor() noexcept {
  for (;;) {
   std::array<pollfd, 3U> descriptors{{
    {.fd = custody_.poll_fd(), .events = POLLIN, .revents = 0},
    {.fd = stop_fd_.get(), .events = POLLIN, .revents = 0},
    {.fd = timer_fd_.get(), .events = POLLIN, .revents = 0},
   }};
   const int result = ::poll(descriptors.data(), descriptors.size(), -1);
   if (result < 0) {
    if (errno == EINTR) continue;
    terminal_monitor_failure("child.monitor_wait_refused", errno, errno);
    return;
   }
   constexpr short failed = POLLERR | POLLHUP | POLLNVAL;
   if ((descriptors[1].revents & failed) != 0 || (descriptors[2].revents & failed) != 0 || (descriptors[0].revents & POLLNVAL) != 0) {
    terminal_monitor_failure("child.monitor_descriptor_refused", EIO);
    return;
   }
   if ((descriptors[1].revents & POLLIN) != 0 && !begin_stop()) return;
   if ((descriptors[2].revents & POLLIN) != 0) {
    const auto consumed = mmltk::common::io::read_counter_fd(timer_fd_.get());
    if (consumed.bytes != static_cast<ssize_t>(sizeof(consumed.count))) {
     terminal_monitor_failure("child.stop_timer_read_refused", consumed.bytes < 0 ? consumed.error : EIO, consumed.bytes < 0 ? consumed.error : 0);
     return;
    }
    disarm_timer();
    static_cast<void>(force_stop());
    return;
   }
   if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
    if (!reap()) return;
   }
  }
 }
 [[nodiscard]] bool begin_stop() noexcept {
  static_cast<void>(mmltk::common::io::read_counter_fd(stop_fd_.get()));
  trace("stop.term_selected");
  bool signaled = false;
  {
   std::scoped_lock lock{mutex_};
   signaled = custody_.Signal(SIGTERM);
   if (!signaled) record_infrastructure_failure_locked(errno, errno);
  }
  if (!signaled) { return force_stop(); }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(stop_grace_);
  const auto nanoseconds = stop_grace_ - seconds;
  const itimerspec deadline{.it_interval = {},
                            .it_value = {
                             .tv_sec = static_cast<time_t>(seconds.count()),
                             .tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(nanoseconds).count()),
                            }};
  if (::timerfd_settime(timer_fd_.get(), 0, &deadline, nullptr) != 0) {
   terminal_monitor_failure("child.stop_timer_arm_refused", errno, errno);
   return false;
  }
  return true;
 }
 void disarm_timer() noexcept {
  const itimerspec disarmed{};
  static_cast<void>(::timerfd_settime(timer_fd_.get(), 0, &disarmed, nullptr));
 }
 void record_infrastructure_failure_locked(const int status, const int cause) noexcept {
  if (infrastructure_error_) return;
  infrastructure_error_ = status != 0 ? status : EIO;
  lifecycle_.error_code = cause;
 }
 void terminal_monitor_failure(const std::string_view event, const int error, const int cause = 0) noexcept {
  disarm_timer();
  settle_failure(event, error, cause);
 }
 [[nodiscard]] bool force_stop() noexcept {
  ChildSettlement settlement;
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled() || lifecycle_.kill_selected) return false;
   lifecycle_.kill_selected = true;
   if (!custody_.Signal(SIGKILL)) record_infrastructure_failure_locked(errno, errno);
   settlement = custody_.Settle(diagnostics_);
  }
  trace("stop.kill_selected");
  publish_settlement(settlement.child);
  return false;
 }
 [[nodiscard]] bool reap() noexcept {
  disarm_timer();
  ChildSettlement settlement;
  {
   std::scoped_lock lock{mutex_};
   settlement = custody_.Settle(diagnostics_);
  }
  publish_settlement(settlement.child);
  return false;
 }
 void publish_settlement(const siginfo_t child, const int infrastructure_error = 0) noexcept {
  int failure = infrastructure_error;
  {
   std::scoped_lock lock{mutex_};
   if (failure == 0 && infrastructure_error_) failure = *infrastructure_error_;
  }
  if (failure != 0) {
   publish_infrastructure_failure(failure);
   return;
  }
  publish_terminal(child.si_code == CLD_EXITED ? FirefoxProcessTerminal::Exited : FirefoxProcessTerminal::Signaled,
                   child.si_code == CLD_EXITED ? child.si_status : 128 + child.si_status);
 }
 void settle_failure(const std::string_view event, const int error, const int cause = 0) noexcept {
  trace(event, static_cast<std::uint64_t>(static_cast<std::uint32_t>(error)));
  ChildSettlement settlement;
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled()) return;
   record_infrastructure_failure_locked(error, cause);
   lifecycle_.kill_selected = true;
   static_cast<void>(custody_.Signal(SIGKILL));
   settlement = custody_.Settle(diagnostics_);
  }
  publish_settlement(settlement.child);
 }
 void publish_infrastructure_failure(const int error) noexcept {
  FirefoxProcessLifecycle published;
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled()) return;
   lifecycle_.terminal = FirefoxProcessTerminal::StartupFailed;
   lifecycle_.status = error != 0 ? error : 1;
   published = lifecycle_;
   stop_fd_.reset();
   timer_fd_.reset();
  }
  observations_.publish({.kind = FirefoxPhysicalObservationKind::InfrastructureFailure, .process = published});
 }
 void publish_terminal(FirefoxProcessTerminal terminal, int status) noexcept {
  FirefoxProcessLifecycle published;
  {
   std::scoped_lock lock{mutex_};
   if (lifecycle_.settled()) return;
   lifecycle_.terminal = terminal;
   lifecycle_.status = status;
   published = lifecycle_;
   stop_fd_.reset();
   timer_fd_.reset();
  }
  trace(terminal == FirefoxProcessTerminal::Exited ? "child.exited" : "child.signaled", static_cast<std::uint64_t>(static_cast<std::uint32_t>(status)));
  observations_.publish({.kind = FirefoxPhysicalObservationKind::ProcessTerminal, .process = published});
 }
 [[nodiscard]] bool valid_launch_inputs() const noexcept {
  if (executable_.empty() || page_url_.empty() || stop_grace_ <= std::chrono::milliseconds::zero() || stop_grace_ > kMaximumFirefoxStopGrace ||
      !runtime_signals_are_blocked())
   return false;
  try {
   return std::filesystem::is_regular_file(executable_);
  } catch (...) { return false; }
 }
 [[nodiscard]] bool prepare_profile() noexcept {
  try {
   const char* runtime = std::getenv("XDG_RUNTIME_DIR");
   const std::filesystem::path root = runtime && runtime[0] ? std::filesystem::path{runtime} : std::filesystem::temp_directory_path();
   std::string pattern = (root / "mmltk-firefox-XXXXXX").string();
   std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
   mutable_pattern.push_back('\0');
   char* created = ::mkdtemp(mutable_pattern.data());
   if (!created) return false;
   profile_ = created;
   if (::chmod(profile_.c_str(), S_IRWXU) != 0) return false;
   return true;
  } catch (...) {
   cleanup_profile();
   return false;
  }
 }
 void cleanup_profile() noexcept {
  if (profile_.empty()) return;
  std::error_code error;
  if (mmltk::common::io::remove_tree_no_follow(profile_, error) && !error) profile_.clear();
 }
 void trace(std::string_view event, std::uint64_t value = 0U) const noexcept {
  if (!diagnostics_.valid()) return;
  pid_t process = -1;
  {
   std::scoped_lock lock{mutex_};
   process = custody_.pid();
  }
  diagnostics_.Emit([&] {
   return RuntimeDiagnosticFact{
    .owner = contracts::DiagnosticOwner::FirefoxProcess, .event = event, .sequence = process > 0 ? static_cast<std::uint64_t>(process) : 0U, .value = value};
  });
 }
 [[nodiscard]] bool prepare_log_handoff(SpawnFileActions& actions) noexcept {
  if (log_file_.empty()) return true;
  firefox_log_.reset(::open(log_file_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR));
  if (firefox_log_.get() < 0) return false;
  return actions.initialize() && actions.add_dup2(firefox_log_.get(), STDOUT_FILENO) && actions.add_dup2(firefox_log_.get(), STDERR_FILENO) &&
         actions.add_close(firefox_log_.get());
 }
 FirefoxProcessStartResult startup_failed(std::string_view event, const int error = 0) noexcept {
  trace(event, static_cast<std::uint64_t>(error));
  FirefoxProcessLifecycle published;
  {
   std::scoped_lock lock{mutex_};
   lifecycle_.terminal = FirefoxProcessTerminal::StartupFailed;
   lifecycle_.status = 1;
   lifecycle_.error_code = error;
   published = lifecycle_;
  }
  cleanup_profile();
  observations_.publish({.kind = FirefoxPhysicalObservationKind::ProcessTerminal, .process = published});
  return FirefoxProcessStartResult::Terminal;
 }
 const std::filesystem::path workspace_import_socket_;
 const std::filesystem::path executable_;
 const std::string page_url_;
 const std::filesystem::path log_file_;
 const FirefoxProcessObservationTarget observations_;
 const RuntimeDiagnosticTarget diagnostics_;
 const std::chrono::milliseconds stop_grace_;
 const bool integration_;
 const bool integration_high_dpi_;
 mutable std::mutex mutex_;
 std::filesystem::path profile_;
 ChildCustody custody_;
 ScopedFd stop_fd_;
 ScopedFd timer_fd_;
 ScopedFd firefox_log_;
 std::jthread monitor_;
 std::optional<int> infrastructure_error_;
 FirefoxProcessLifecycle lifecycle_{};
};
bool block_browser_runtime_signals() noexcept {
 const sigset_t signals = runtime_signal_set();
 return ::pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
}
FirefoxProcessOwner::FirefoxProcessOwner(std::filesystem::path workspace_import_socket, FirefoxProcessConfig config,
                                         const FirefoxProcessObservationTarget observations, const RuntimeDiagnosticTarget diagnostics) noexcept
    : observations_(observations),
      implementation_(new (std::nothrow) Implementation(std::move(workspace_import_socket), std::move(config), observations, diagnostics)) {}
FirefoxProcessOwner::FirefoxProcessOwner(const FirefoxProcessObservationTarget observations) noexcept : observations_(observations) {}
FirefoxProcessOwner::~FirefoxProcessOwner() noexcept = default;
FirefoxProcessStartResult FirefoxProcessOwner::start() noexcept {
 if (implementation_) return implementation_->start();
 if (!fallback_published_) {
  fallback_published_ = true;
  observations_.publish({.kind = FirefoxPhysicalObservationKind::ProcessTerminal, .process = fallback_lifecycle_});
 }
 return FirefoxProcessStartResult::Terminal;
}
void FirefoxProcessOwner::request_stop() noexcept {
 if (implementation_) implementation_->request_stop();
}
void FirefoxProcessOwner::wait() noexcept {
 if (implementation_) implementation_->wait();
}
FirefoxProcessLifecycle FirefoxProcessOwner::lifecycle() const noexcept { return implementation_ ? implementation_->lifecycle() : fallback_lifecycle_; }
void FirefoxProcessOwner::prefer_retained_pid_wait_for_test() noexcept {
 if (implementation_) implementation_->prefer_retained_pid_wait_for_test();
}
int browser_runtime_exit_status(const FirefoxProcessLifecycle firefox, const bool application_healthy) noexcept {
 if (!application_healthy) return firefox.status != 0 ? firefox.status : 1;
 if (firefox.terminal == FirefoxProcessTerminal::Exited) return firefox.status;
 if (firefox.terminal == FirefoxProcessTerminal::Signaled && firefox.stop_requested && !firefox.kill_selected && firefox.status == 128 + SIGTERM) return 0;
 return firefox.status != 0 ? firefox.status : 1;
}
}  // namespace mmltk::controller::services
