module;
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <ctime>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <array>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
export module mmltk.tools.run_traced_command;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
namespace support = mmltk::tools;
volatile std::sig_atomic_t pending_signal = 0;
void defer_signal(int signal) {
 if (pending_signal == 0) pending_signal = signal;
}
void install_handler(int signal, void (*handler)(int), const char* failure) {
 struct sigaction action{};
 action.sa_handler = handler;
 ::sigemptyset(&action.sa_mask);
 if (::sigaction(signal, &action, nullptr) != 0) throw std::system_error(errno, std::generic_category(), failure);
}
// Like a shell waiting for a foreground command, remain in the inherited
// process group and defer termination until the foreground child settles.
// Spawn resets caught dispositions in the child; inherited ignores survive.
class ForegroundSignals {
public:
 ForegroundSignals() {
  for (const int signal : {SIGHUP, SIGINT, SIGQUIT, SIGTERM}) {
   struct sigaction previous{};
   if (::sigaction(signal, nullptr, &previous) != 0) throw std::system_error(errno, std::generic_category(), "sigaction");
   if (previous.sa_handler == SIG_IGN) continue;
   install_handler(signal, defer_signal, "sigaction");
  }
 }
};
void trace_event(std::string_view phase, std::string_view event, std::string_view result) {
 const auto path = support::env("MMLTK_BUILD_TRACE_FILE");
 if (path.empty()) return;
 // Bash printf %d accepts base prefixes and quoted character constants.
 auto jobs_text = support::env("MMLTK_JOBS", "0");
 if (jobs_text.empty()) jobs_text = "0";
 std::intmax_t jobs = 0;
 bool valid_jobs = true;
 if (jobs_text.front() == '\'' || jobs_text.front() == '"') {
  if (jobs_text.size() > 1) jobs = static_cast<unsigned char>(jobs_text[1]);
 } else {
  char* end = nullptr;
  errno = 0;
  jobs = std::strtoimax(jobs_text.c_str(), &end, 0);
  valid_jobs = end != jobs_text.c_str() && *end == '\0' && errno != ERANGE;
 }
 struct timespec now{};
 if (::clock_gettime(CLOCK_REALTIME, &now) != 0) throw std::system_error(errno, std::generic_category(), "trace timestamp");
 const auto timestamp = static_cast<std::intmax_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
 const support::json record = {{"timestamp_ns", timestamp}, {"pid", ::getpid()}, {"phase", phase}, {"event", event}, {"result", result}, {"jobs", jobs}};
 const std::string line = record.dump() + '\n';
 support::FileDescriptor output(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666));
 if (output.get() < 0) throw std::system_error(errno, std::generic_category(), "trace append " + path);
 support::write_all(output.get(), line, "trace write " + path);
 // printf emits the converted record but returns failure for invalid input.
 if (!valid_jobs) throw support::ToolError("MMLTK_JOBS: invalid number: " + jobs_text);
}
int run(int argc, char** argv) {
 if (argc < 3) {
  std::cerr << "usage: " << argv[0] << " <phase> <command> [args...]\n";
  return 2;
 }
 const std::string_view phase(argv[1]);
 if (phase.empty() || phase.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.:-") != std::string_view::npos) {
  std::cerr << "invalid trace phase: " << phase << '\n';
  return 2;
 }
 trace_event(phase, "start", "");
 ForegroundSignals signals;
 int status = 1;
 int signal = 0;
 try {
  const std::vector<std::string> command(argv + 2, argv + argc);
  if (command.front().empty()) {
   std::cerr << "run_traced_command: : command not found\n";
   status = 127;
  } else {
   const auto child = support::run(command);
   status = child.exit_code;
   // A foreground application may handle Ctrl-C itself. Bash only
   // terminates for SIGINT when that application also died of SIGINT.
   signal = pending_signal;
   if (signal == SIGINT && child.signal != SIGINT) signal = 0;
   if (signal != 0) status = 128 + signal;
  }
 } catch (const std::exception& error) { std::cerr << "run_traced_command: " << error.what() << '\n'; }
 trace_event(phase, "finish", status == 0 ? "success" : "failure:" + std::to_string(status));
 if (signal != 0) {
  install_handler(signal, SIG_DFL, "restore terminating signal");
  ::raise(signal);
 }
 return status;
}
}  // namespace
export int mmltk_tool_run_traced_command(int argc, char** argv) {
 try {
  return run(argc, argv);
 } catch (const support::ToolError& error) {
  std::cerr << "run_traced_command: " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << "run_traced_command: " << error.what() << '\n';
  return 1;
 }
}
