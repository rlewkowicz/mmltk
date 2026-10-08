module;
// Native counterpart of test_headless_compositor.py; no GPU is required.
// Link tools/headless_compositor.cpp with MMLTK_HEADLESS_COMPOSITOR_NO_MAIN
// and the existing third_party/json/include include directory. The wrapper
// must retain core=1:1 for this executable and its self-executed fixtures.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <climits>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <sys/stat.h>
#include <sched.h>
export module mmltk.tools.tests.test_headless_compositor;
import mmltk.tools.tool_command;
import mmltk.tools.container_commands;
import mmltk.tools.cpp_linux;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.headless_compositor;
import mmltk.tools.tests.cpp_test_support;
namespace {
namespace native = mmltk::tools;
namespace runner = native::headless;
namespace fs = std::filesystem;
using native::json;
using namespace std::chrono_literals;
constexpr std::string_view HARDWARE_LOG =
 "EGL vendor: NVIDIA\nGL vendor: NVIDIA Corporation\n"
 "GL renderer: NVIDIA GeForce RTX\n    dmabuf support: modifiers\n";
constexpr std::string_view PROTOCOLS =
 "interface: 'wl_compositor', version: 4, name: 1\n"
 "interface: 'wl_output', version: 4, name: 1\n"
 "interface: 'xdg_wm_base', version: 4, name: 1\n"
 "interface: 'zwp_linux_dmabuf_v1', version: 4, name: 1\n"
 "interface: 'wl_seat', version: 7, name: 5\n"
 "\tname: default\n\tcapabilities: pointer keyboard";
void require(bool condition, std::string_view message) {
 if (!condition) throw std::runtime_error(std::string(message));
}
void system_check(bool condition, std::string_view operation) {
 if (!condition) throw std::system_error(errno, std::generic_category(), std::string(operation));
}
template <class Callable>
void requires_session_failure(Callable&& callable) {
 try {
  std::forward<Callable>(callable)();
 } catch (const runner::SessionFailure&) { return; }
 throw std::runtime_error("expected SessionFailure");
}
std::string replaced(std::string_view text, std::string_view old, std::string_view value) {
 std::string result(text);
 const auto position = result.find(old);
 require(position != std::string::npos, "replacement fixture text is missing");
 result.replace(position, old.size(), value);
 return result;
}
runner::Command fixture_command(std::initializer_list<std::string> arguments) {
 runner::Command result = native::command::self_invocation(native::command::Command::test_headless_compositor, {"--fixture"});
 result.insert(result.end(), arguments.begin(), arguments.end());
 return result;
}
void exit_cleanly(int) { ::_exit(0); }
void exit_compositor_lost(int) { ::_exit(73); }
void install_handler(int number, void (*handler)(int)) {
 struct sigaction action{};
 action.sa_handler = handler;
 ::sigemptyset(&action.sa_mask);
 system_check(::sigaction(number, &action, nullptr) == 0, "fixture sigaction");
}
[[noreturn]] void await_signal() {
 for (;;) ::pause();
}
int compositor_fixture(const runner::Command& arguments) {
 require(arguments.size() >= 3, "compositor fixture requires mode and hardware log");
 require(
  std::find(arguments.begin(), arguments.end(), "--modules=/usr/lib/weston/mmltk-headless-seat.so,systemd-notify.so") != arguments.end(), "Weston command did not load the seat and readiness modules");
 const auto log_argument = std::find_if(arguments.begin(), arguments.end(), [](const auto& value) { return value.starts_with("--log="); });
 require(log_argument != arguments.end(), "Weston command has no log path");
 install_handler(SIGTERM, exit_cleanly);
 install_handler(SIGUSR1, exit_compositor_lost);
 native::write_text(fs::path(native::env("XDG_RUNTIME_DIR")) / "compositor.pid", std::to_string(::getpid()));
 native::write_text(log_argument->substr(6), arguments[2]);
 if (arguments[1] == "fail") return 19;
 if (arguments[1] != "silent") {
  native::FileDescriptor ready(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  system_check(ready.get() >= 0, "fixture readiness socket");
  const auto notification = native::env("NOTIFY_SOCKET");
  const native::UnixAddress address(notification, "fixture notify path too long");
  constexpr std::string_view message = "READY=1";
  system_check(::sendto(ready.get(), message.data(), message.size(), 0, address.get(), address.length) == static_cast<ssize_t>(message.size()), "fixture readiness notification");
 }
 await_signal();
}
int descendant_fixture(const runner::Command& arguments) {
 require(arguments.size() == 3, "descendant fixture requires mode and PID path");
 int descriptors[2];
 system_check(::pipe2(descriptors, O_CLOEXEC) == 0, "fixture descendant pipe");
 native::FileDescriptor read_end(descriptors[0]);
 native::FileDescriptor write_end(descriptors[1]);
 const pid_t child = ::fork();
 system_check(child >= 0, "fixture fork descendant");
 if (child == 0) {
  read_end.reset();
  install_handler(SIGTERM, arguments[1] == "stubborn" ? SIG_IGN : exit_cleanly);
  constexpr char ready = 'R';
  ssize_t count;
  do { count = ::write(write_end.get(), &ready, 1); } while (count < 0 && errno == EINTR);
  if (count != 1) ::_exit(92);
  write_end.reset();
  await_signal();
 }
 write_end.reset();
 char ready = 0;
 ssize_t count;
 do { count = ::read(read_end.get(), &ready, 1); } while (count < 0 && errno == EINTR);
 read_end.reset();
 require(count == 1 && ready == 'R', "descendant did not install its signal handler");
 native::write_text(arguments[2], std::to_string(child));
 return 0;  // Leave the live descendant in the exited leader's process group.
}
int run_fixture(const runner::Command& arguments) {
 require(!arguments.empty(), "fixture mode is required");
 const auto& mode = arguments.front();
 if (mode == "compositor") return compositor_fixture(arguments);
 if (mode == "probe") {
  require(arguments.size() == 2, "probe fixture requires a protocol report");
  std::cout << arguments[1] << '\n';
  return 0;
 }
 if (mode == "exit") {
  require(arguments.size() == 2, "exit fixture requires a status");
  return std::stoi(arguments[1]);
 }
 if (mode == "abort") std::abort();
 if (mode == "kill-self") {
  system_check(::kill(::getpid(), SIGKILL) == 0, "fixture kill self");
  await_signal();
 }
 if (mode == "lose-compositor") {
  const auto pid = static_cast<pid_t>(std::stol(native::read_text(fs::path(native::env("XDG_RUNTIME_DIR")) / "compositor.pid")));
  system_check(::kill(pid, SIGUSR1) == 0, "fixture kill compositor");
  await_signal();
 }
 if (mode == "cancel" || mode == "stubborn-cancel") {
  if (mode == "stubborn-cancel") install_handler(SIGTERM, SIG_IGN);
  system_check(::kill(::getppid(), SIGTERM) == 0, "fixture cancel supervisor");
  await_signal();
 }
 if (mode == "descendant") return descendant_fixture(arguments);
 throw std::runtime_error("unknown fixture mode: " + mode);
}
class SessionFixture {
public:
 SessionFixture() : artifacts(temporary.path() / "artifacts") { fs::create_directory(artifacts); }
 int run(const runner::Command& command) {
  runner::SessionOptions options;
  options.environment["XDG_RUNTIME_DIR"] = temporary.path().string();
  options.runtime_parent = temporary.path();
  options.weston_config = repository() / "tools/headless-weston.ini";
  options.weston_command = fixture_command({"compositor", mode, log});
  options.probe_command = fixture_command({"probe", protocols});
  options.startup_timeout = 300ms;
  options.shutdown_timeout = 300ms;
  session = std::make_unique<runner::HeadlessSession>(artifacts, std::move(options));
  const int result = session->run(command);
  children = session->children();
  // Preserve asyncTearDown's checks for every session case, including
  // failed startup, cancellation, and failed command spawning.
  for (const auto& child : children) require(child.returncode.has_value(), "unreaped child " + std::to_string(child.pid));
  for (const auto& entry : fs::directory_iterator(temporary.path())) require(!entry.path().filename().string().starts_with("headless-"), "runtime directory leaked");
  return result;
 }
 int run_fixture(std::initializer_list<std::string> arguments) { return run(fixture_command(arguments)); }
 std::vector<json> events() const {
  std::vector<json> result;
  for (const auto& line : native::lines(native::read_text(artifacts / "supervisor.jsonl"))) result.push_back(json::parse(line));
  return result;
 }
 bool has_event(std::string_view name) const {
  const auto records = events();
  return std::any_of(records.begin(), records.end(), [&](const auto& record) { return record.at("event") == name; });
 }
 std::string failure_message() const {
  for (const auto& event : events())
   if (event.at("event") == "headless.failed") return event.at("message").get<std::string>();
  throw std::runtime_error("headless.failed event is missing");
 }
 static fs::path repository() {
  if (const auto root = native::optional_env("MMLTK_REPO_ROOT")) return *root;
  return native::repository_root({}, __FILE__);
 }
 native::TemporaryDirectory temporary{"mmltk-headless-test-"};
 fs::path artifacts;
 std::string mode = "ready";
 std::string log{HARDWARE_LOG};
 std::string protocols{PROTOCOLS};
 std::vector<runner::ChildStatus> children;

private:
 std::unique_ptr<runner::HeadlessSession> session;
};
void test_wrapper_disables_os_core_reporting_for_fixture_children() {
 rlimit limits{};
 system_check(::getrlimit(RLIMIT_CORE, &limits) == 0, "getrlimit core");
 require(limits.rlim_cur == 1 && limits.rlim_max == 1, "wrapper must set RLIMIT_CORE to (1, 1)");
}
void test_fatal_signal_preserves_status_without_an_os_core_dump() {
 auto command = fixture_command({"abort"});
 std::vector<char*> arguments;
 for (auto& argument : command) arguments.push_back(argument.data());
 arguments.push_back(nullptr);
 pid_t pid = -1;
 const int error = ::posix_spawn(&pid, command.front().c_str(), nullptr, nullptr, arguments.data(), ::environ);
 if (error) throw std::system_error(error, std::generic_category(), "spawn abort fixture");
 int status = 0;
 pid_t waited;
 do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
 system_check(waited == pid, "waitpid abort fixture");
 require(WIFSIGNALED(status), "abort fixture must exit through a signal");
 require(WTERMSIG(status) == SIGABRT, "abort fixture must preserve SIGABRT");
 require(!WCOREDUMP(status), "abort fixture produced an OS core dump");
}
void test_packaged_enumeration_bypasses_compositor_but_execution_owns_one() {
 for (const std::string argument : {"--list-tests", "workspace_wayland_retained"}) {
  const auto command = mmltk::container_commands::workspace_test_command(true, "/packaged/test", {argument}, "packaged", "validation");
  const auto has = [&](std::string_view value) { return std::find(command.begin(), command.end(), value) != command.end(); };
  require(has("headless-compositor") == (argument != "--list-tests"), "wrapper compositor ownership differs for enumeration/execution");
  require(has("validation") == (argument != "--list-tests"), "wrapper selected the wrong image");
  require(has("packaged") == (argument == "--list-tests"), "enumeration must use the packaged runtime");
  require(has("core=1:1") == (argument != "--list-tests"), "private compositor lost its core limit");
  require(has("/packaged/test") && has(argument), "wrapper lost the executable or its argument");
 }
}
void test_requires_nvidia_vendor_and_modifier_support() {
 require(runner::hardware_renderer(HARDWARE_LOG).at("GL vendor") == "NVIDIA Corporation", "NVIDIA GL vendor must be retained");
 for (const auto& log :
  {replaced(HARDWARE_LOG, "EGL vendor: NVIDIA", "EGL vendor: Mesa"), replaced(HARDWARE_LOG, "NVIDIA GeForce RTX", "llvmpipe"),
   replaced(HARDWARE_LOG, "dmabuf support: modifiers", "dmabuf support: basic"), std::string{}})
  requires_session_failure([&] { runner::hardware_renderer(log); });
}
void test_requires_real_wayland_dmabuf_output_and_shell() {
 runner::require_wayland_protocols(PROTOCOLS);
 for (const auto& line : native::lines(PROTOCOLS)) {
  if (!line.starts_with("interface:")) continue;
  requires_session_failure([&] { runner::require_wayland_protocols(replaced(PROTOCOLS, line, "")); });
 }
}
void test_requires_pointer_and_keyboard_on_one_seat() {
 for (const auto* replacement : {"pointer", "keyboard", "", "pointer keyboardish"})
  requires_session_failure([&] { runner::require_wayland_protocols(replaced(PROTOCOLS, "pointer keyboard", replacement)); });
 const auto separate_seats = replaced(PROTOCOLS, "pointer keyboard", "pointer") + "\ninterface: 'wl_seat', version: 7, name: 6\n\tcapabilities: keyboard";
 requires_session_failure([&] { runner::require_wayland_protocols(separate_seats); });
 const auto wrong_global = replaced(PROTOCOLS, "pointer keyboard", "pointer") + "\ninterface: 'other', version: 1, name: 6\n\tcapabilities: pointer keyboard";
 requires_session_failure([&] { runner::require_wayland_protocols(wrong_global); });
 runner::require_wayland_protocols(replaced(PROTOCOLS, "pointer keyboard", "keyboard pointer touch"));
}
void test_private_environment_owns_display_and_renderer() {
 auto inherited = runner::current_environment();
 const runner::Environment injected{
  {"DISPLAY", ":0"}, {"MOZ_HEADLESS", "1"}, {"WAYLAND_SOCKET", "4"}, {"DBUS_SESSION_BUS_ADDRESS", "host"}, {"LIBGL_ALWAYS_SOFTWARE", "1"}, {"__EGL_VENDOR_LIBRARY_FILENAMES", "mesa.json"},
  {"MMLTK_GUI_PIXEL_TRACE", "1"}
 };
 for (const auto& [key, value] : injected) inherited[key] = value;
 const auto environment = runner::session_environment("/tmp/private", std::move(inherited));
 require(environment.at("XDG_RUNTIME_DIR") == "/tmp/private", "session must own its runtime path");
 require(environment.at("MMLTK_GUI_PIXEL_TRACE") == "1", "pixel trace option must survive isolation");
 require(environment.at("__EGL_VENDOR_LIBRARY_FILENAMES").ends_with("10_nvidia.json"), "session must select the NVIDIA EGL vendor");
 for (const auto* name : {"DISPLAY", "MOZ_HEADLESS", "WAYLAND_SOCKET", "DBUS_SESSION_BUS_ADDRESS", "LIBGL_ALWAYS_SOFTWARE"})
  require(!environment.contains(name), "session retained host environment variable " + std::string(name));
}
void test_normal_completion_and_command_status() {
 SessionFixture fixture;
 require(fixture.run_fixture({"exit", "7"}) == 7, "command status must propagate");
 const auto events = fixture.events();
 require(fixture.has_event("headless.ready"), "session readiness must be recorded");
 require(events.size() >= 2, "session lifecycle log is incomplete");
 require(events[events.size() - 2].at("returncode") == 0, "compositor shutdown must be clean");
 require(events.back().at("exit_code") == 7, "finished event must retain command status");
}
void test_command_death_propagates_without_a_session_deadline() {
 SessionFixture fixture;
 require(fixture.run_fixture({"kill-self"}) == 137, "SIGKILL command status must propagate");
 require(fixture.events().back().at("exit_code") == 137, "finished event must record SIGKILL status");
}
void test_success_requires_clean_compositor_shutdown() {
 SessionFixture fixture;
 require(fixture.run_fixture({"exit", "0"}) == 0, "clean command and compositor shutdown must succeed");
 require(fixture.events().back().at("exit_code") == 0, "finished event must record success");
}
void test_startup_failure_does_not_launch_client() {
 SessionFixture fixture;
 fixture.mode = "fail";
 require(fixture.run_fixture({"exit", "0"}) == 1, "Weston startup failure must fail the session");
 require(fixture.children.size() == 1, "startup failure must not launch probe or client");
 require(fixture.failure_message().find("19") != std::string::npos, "startup failure lost Weston status 19");
}
void test_missing_ready_times_out_and_reaps() {
 SessionFixture fixture;
 fixture.mode = "silent";
 require(fixture.run_fixture({"exit", "0"}) == 1, "missing readiness must time out");
 require(fixture.children.size() == 1, "missing readiness must not launch probe or client");
}
void test_software_renderer_prevents_client_start() {
 SessionFixture fixture;
 fixture.log = replaced(HARDWARE_LOG, "NVIDIA GeForce RTX", "llvmpipe");
 require(fixture.run_fixture({"exit", "0"}) == 1, "software renderer must fail admission");
 require(fixture.children.size() == 1, "software renderer must not launch probe or client");
}
void test_missing_dmabuf_prevents_acceptance_start() {
 SessionFixture fixture;
 fixture.protocols = replaced(PROTOCOLS, "zwp_linux_dmabuf_v1", "missing_protocol");
 require(fixture.run_fixture({"exit", "0"}) == 1, "missing DMA-BUF must fail admission");
 require(fixture.children.size() == 2, "missing DMA-BUF must launch only compositor and probe");
}
void test_compositor_loss_aborts_running_test() {
 SessionFixture fixture;
 require(fixture.run_fixture({"lose-compositor"}) == 1, "compositor loss must abort the running test");
 require(fixture.failure_message().find("73") != std::string::npos, "compositor loss must report status 73");
}
void test_cancellation_terminates_child() {
 SessionFixture fixture;
 require(fixture.run_fixture({"cancel"}) == 143, "supervisor SIGTERM must propagate");
 require(!fixture.children.empty(), "cancelled session launched no child");
 require(fixture.children.back().returncode == -SIGTERM, "cancellation must terminate the client with SIGTERM");
}
void test_stubborn_child_is_killed_after_grace_period() {
 SessionFixture fixture;
 require(fixture.run_fixture({"stubborn-cancel"}) == 143, "cancellation must retain supervisor SIGTERM status");
 require(!fixture.children.empty(), "cancelled session launched no child");
 require(fixture.children.back().returncode == -SIGKILL, "stubborn client must be killed after grace period");
 require(fixture.has_event("headless.cleanup.escalated"), "cleanup escalation must be recorded");
}
void check_exited_leader_descendant(bool stubborn) {
 SessionFixture fixture;
 const auto pid_file = fixture.artifacts / "descendant.pid";
 struct DescendantCleanup {
  fs::path pid_file;
  ~DescendantCleanup() {
   try {
    if (fs::exists(pid_file)) ::kill(static_cast<pid_t>(std::stol(native::read_text(pid_file))), SIGKILL);
   } catch (...) {}  // Best-effort backstop also runs after a failed assertion.
  }
 } cleanup{pid_file};
 const int result = fixture.run_fixture({"descendant", stubborn ? "stubborn" : "cooperative", pid_file.string()});
 require(result == static_cast<int>(stubborn), "descendant shutdown must determine clean session status");
 require(fixture.has_event("headless.cleanup.escalated") == stubborn, "cleanup must escalate exactly for the stubborn descendant");
 const auto pid = static_cast<pid_t>(std::stol(native::read_text(pid_file)));
 native::FileDescriptor descriptor(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
 if (descriptor.get() < 0 && errno == ESRCH) return;
 system_check(descriptor.get() >= 0, "pidfd_open descendant");
 pollfd event{descriptor.get(), POLLIN, 0};
 int ready;
 do { ready = ::poll(&event, 1, 0); } while (ready < 0 && errno == EINTR);
 system_check(ready >= 0, "poll descendant");
 require(ready > 0 && (event.revents & POLLIN), "descendant is still running");
}
void test_exited_leader_does_not_abandon_stubborn_descendant() { check_exited_leader_descendant(true); }
void test_exited_leader_allows_descendant_to_finish_on_term() { check_exited_leader_descendant(false); }
void test_missing_command_cleans_started_compositor() {
 SessionFixture fixture;
 require(fixture.run({"/no/such/mmltk-test-command"}) == 1, "missing command must fail and retire the compositor");
}
void test_signal_is_propagated_and_all_children_retire() {
 SessionFixture fixture;
 require(fixture.run_fixture({"cancel"}) == 143, "SIGTERM must propagate after all children retire");
 require(fixture.events().back().at("exit_code") == 143, "finished event must retain SIGTERM status");
}
struct TestCase {
 std::string_view name;
 void (*run)();
};
#define HEADLESS_CASE(name) \
 TestCase { #name, name }
constexpr std::array cases{
 HEADLESS_CASE(test_wrapper_disables_os_core_reporting_for_fixture_children),
 HEADLESS_CASE(test_fatal_signal_preserves_status_without_an_os_core_dump),
 HEADLESS_CASE(test_packaged_enumeration_bypasses_compositor_but_execution_owns_one),
 HEADLESS_CASE(test_requires_nvidia_vendor_and_modifier_support),
 HEADLESS_CASE(test_requires_real_wayland_dmabuf_output_and_shell),
 HEADLESS_CASE(test_requires_pointer_and_keyboard_on_one_seat),
 HEADLESS_CASE(test_private_environment_owns_display_and_renderer),
 HEADLESS_CASE(test_normal_completion_and_command_status),
 HEADLESS_CASE(test_command_death_propagates_without_a_session_deadline),
 HEADLESS_CASE(test_success_requires_clean_compositor_shutdown),
 HEADLESS_CASE(test_startup_failure_does_not_launch_client),
 HEADLESS_CASE(test_missing_ready_times_out_and_reaps),
 HEADLESS_CASE(test_software_renderer_prevents_client_start),
 HEADLESS_CASE(test_missing_dmabuf_prevents_acceptance_start),
 HEADLESS_CASE(test_compositor_loss_aborts_running_test),
 HEADLESS_CASE(test_cancellation_terminates_child),
 HEADLESS_CASE(test_stubborn_child_is_killed_after_grace_period),
 HEADLESS_CASE(test_exited_leader_does_not_abandon_stubborn_descendant),
 HEADLESS_CASE(test_exited_leader_allows_descendant_to_finish_on_term),
 HEADLESS_CASE(test_missing_command_cleans_started_compositor),
 HEADLESS_CASE(test_signal_is_propagated_and_all_children_retire),
};
#undef HEADLESS_CASE
}  // namespace
export int mmltk_tool_test_headless_compositor(int argc, char** argv) {
 try {
  if (argc >= 2 && std::string_view(argv[1]) == "--fixture") return run_fixture({argv + 2, argv + argc});
  if (argc == 2 && std::string_view(argv[1]) == "--list-tests") return native::testing::list_cases(cases);
  require(argc <= 2, "usage: test_headless_compositor [--list-tests | test_name]");
  std::size_t selected = 0;
  std::size_t failed = 0;
  for (const auto& test : cases) {
   if (argc == 2 && test.name != argv[1]) continue;
   ++selected;
   try {
    test.run();
    std::cout << "PASS " << test.name << '\n';
   } catch (const std::exception& error) {
    ++failed;
    std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
   }
  }
  require(selected != 0, "unknown test name");
  std::cout << selected - failed << '/' << selected << " cases passed\n";
  return failed == 0 ? 0 : 1;
 } catch (const std::exception& error) {
  std::cerr << "test_headless_compositor: " << error.what() << '\n';
  return 1;
 }
}
