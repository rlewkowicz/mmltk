module;
// Inspect or terminate one exact native process inside its wrapper container.
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
export module mmltk.tools.diagnose_native_process;
import std;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
namespace support = mmltk::tools;
namespace fs = std::filesystem;
// Python bytes repr chooses double quotes only when that avoids escaping a
// single quote. Diagnostic artifacts remain byte-for-byte escaped, not decoded.
std::string repr(std::string_view data, bool bytes = false) { return (bytes ? "b" : "") + support::text::byte_repr(data, true); }
[[noreturn]] void os_error(int code, const std::optional<std::string>& path = {}) {
 throw support::ToolError("[Errno " + std::to_string(code) + "] " + std::strerror(code) + (path ? ": " + repr(*path) : ""));
}
void usage(std::ostream& output, const std::string& program) {
 const std::string prefix = "usage: " + program + " ";
 output << prefix << "[-h] (--backtrace | --terminate | --read-file)\n" << std::string(prefix.size(), ' ') << "[--pid PID] [--file FILE] executable\n";
}
enum class Action { none, backtrace, terminate, read_file };
struct Arguments {
 Action action = Action::none;
 std::string executable;
 std::optional<std::string> pid;
 std::optional<fs::path> file;
};
// Keep arbitrary-size integer selectors as normalized decimal text: selectors
// outside pid_t's range simply match no process, as with argparse's int type.
std::string integer(std::string_view value) {
 const auto result = support::text::decimal_integer(support::trim(value));
 if (!result) throw support::ToolError("argument --pid: invalid int value: " + repr(value), 2);
 return *result;
}
std::optional<Arguments> parse(int argc, char** argv, const std::string& program) {
 Arguments args;
 bool positional_only = false;
 bool have_executable = false;
 std::vector<std::string> extras;
 std::string action_name;
 const std::array<std::string_view, 6> options{"--help", "--backtrace", "--terminate", "--read-file", "--pid", "--file"};
 for (int index = 1; index < argc; ++index) {
  const std::string item(argv[index]);
  if (!positional_only && item == "--") {
   positional_only = true;
   continue;
  }
  if (!positional_only && (item.starts_with("--") || item == "-h")) {
   const auto equals = item.find('=');
   const auto supplied = item.substr(0, equals);
   std::string option = supplied;
   if (option == "-h") option = "--help";
   std::vector<std::string_view> matches;
   for (auto candidate : options)
    if (candidate.starts_with(option)) matches.push_back(candidate);
   if (matches.size() > 1) {
    std::string message = "ambiguous option: " + supplied + " could match ";
    for (std::size_t i = 0; i < matches.size(); ++i) {
     if (i) message += ", ";
     message += matches[i];
    }
    throw support::ToolError(message, 2);
   }
   if (matches.empty()) {
    extras.push_back(item);
    continue;
   }
   option = matches[0];
   if (option == "--pid" || option == "--file") {
    std::string value;
    if (equals != item.npos)
     value = item.substr(equals + 1);
    else {
     if (index + 1 >= argc) throw support::ToolError("argument " + option + ": expected one argument", 2);
     value = argv[index + 1];
     // argparse accepts negative numeric values as arguments.
     if (value.size() > 1 && value[0] == '-' && !(value[1] >= '0' && value[1] <= '9')) throw support::ToolError("argument " + option + ": expected one argument", 2);
     ++index;
    }
    if (option == "--pid")
     args.pid = integer(value);
    else
     args.file = support::lexical_path(value);
    continue;
   }
   if (equals != item.npos) throw support::ToolError("argument " + (option == "--help" ? "-h/--help" : option) + ": ignored explicit argument " + repr(item.substr(equals + 1)), 2);
   if (option == "--help") {
    usage(std::cout, program);
    std::cout << "\nInspect or terminate one exact native process inside its wrapper container.\n\n"
              << "positional arguments:\n  executable\n\noptions:\n"
              << "  -h, --help   show this help message and exit\n"
              << "  --backtrace\n  --terminate\n  --read-file\n"
              << "  --pid PID    select a container PID when an executable has several workers\n"
              << "  --file FILE  absolute container path of a retained diagnostic artifact\n";
    return std::nullopt;
   }
   if (args.action != Action::none && action_name != option) throw support::ToolError("argument " + option + ": not allowed with argument " + action_name, 2);
   action_name = option;
   args.action = option == "--backtrace" ? Action::backtrace : option == "--terminate" ? Action::terminate : Action::read_file;
  } else if (!positional_only && item.size() > 1 && item[0] == '-' && !(item[1] >= '0' && item[1] <= '9'))
   extras.push_back(item);
  else if (!have_executable) {
   args.executable = item;
   have_executable = true;
  } else
   extras.push_back(item);
 }
 if (!have_executable) throw support::ToolError("the following arguments are required: executable", 2);
 if (args.action == Action::none) throw support::ToolError("one of the arguments --backtrace --terminate --read-file is required", 2);
 if (!extras.empty()) {
  std::string message = "unrecognized arguments:";
  for (const auto& extra : extras) message += " " + extra;
  throw support::ToolError(message, 2);
 }
 if ((args.action == Action::read_file) != args.file.has_value()) throw support::ToolError("--read-file requires --file; other actions do not accept --file", 2);
 if (args.file && !args.file->is_absolute()) throw support::ToolError("--file must be an absolute container path", 2);
 return args;
}
int backtrace(pid_t pid) {
 const std::vector<std::string> command{
  "gdb", "--batch", "--nx", "--quiet", "-p", std::to_string(pid), "-ex", "set pagination off", "-ex", "set print thread-events off", "-ex", "thread apply all bt", "-ex", "detach"
 };
 auto pointers = support::detail::argument_pointers(command);
 pid_t child = -1;
 const int error = ::posix_spawnp(&child, pointers[0], nullptr, nullptr, pointers.data(), ::environ);
 if (error) os_error(error, "gdb");
 struct Child {
  pid_t pid;
  bool waited = false;
  ~Child() {
   if (!waited) {
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
   }
  }
 } guard{child};
 support::FileDescriptor event(static_cast<int>(::syscall(SYS_pidfd_open, child, 0)));
 if (event.get() < 0) os_error(errno);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
 pollfd descriptor{event.get(), POLLIN, 0};
 for (;;) {
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  const int ready = ::poll(&descriptor, 1, static_cast<int>(std::max<long long>(0, remaining.count())));
  if (ready < 0 && errno == EINTR) continue;
  if (ready < 0) os_error(errno);
  if (ready > 0) break;
  std::string display = "[";
  for (std::size_t i = 0; i < command.size(); ++i) {
   if (i) display += ", ";
   display += repr(command[i]);
  }
  display += "]";
  throw support::ToolError("Command '" + display + "' timed out after 35 seconds");
 }
 int status = 0;
 while (::waitpid(child, &status, 0) < 0)
  if (errno != EINTR) os_error(errno);
 guard.waited = true;
 return WIFSIGNALED(status) ? -WTERMSIG(status) : WEXITSTATUS(status);
}
int diagnose(const Arguments& args) {
 std::vector<pid_t> matches;
 const auto inspector = std::to_string(::getpid());
 for (const auto& entry : fs::directory_iterator("/proc")) {
  const auto name = entry.path().filename().string();
  if (name.empty() || name.find_first_not_of("0123456789") != name.npos) continue;
  if (name == inspector) continue;
  std::error_code error;
  const auto executable = fs::canonical(entry.path() / "exe", error);
  if (error) {
   if (error.value() == ENOENT || error.value() == EACCES || error.value() == EPERM || error.value() == ESRCH) continue;
   os_error(error.value(), (entry.path() / "exe").string());
  }
  if (executable.filename() == args.executable && (!args.pid || name == *args.pid)) matches.push_back(static_cast<pid_t>(std::stol(name)));
 }
 if (matches.size() != 1) {
  std::string list = "[";
  for (std::size_t i = 0; i < matches.size(); ++i) {
   if (i) list += ", ";
   list += std::to_string(matches[i]);
  }
  throw support::ToolError("expected exactly one " + args.executable + " process; matching container PIDs: " + list + "]; use --pid to select one", 2);
 }
 const pid_t pid = matches.front();
 support::FileDescriptor process(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0)));
 if (process.get() < 0) os_error(errno);
 std::cout << "native process: " << args.executable << ", container pid=" << pid << std::endl;
 if (args.action == Action::read_file) {
  // A double-slash pathlib anchor is not relative to '/'.
  if (args.file->string().starts_with("//")) throw support::ToolError(repr(args.file->string()) + " is not in the subpath of '/' OR one path is relative and the other is absolute.");
  const auto path = fs::path("/proc") / std::to_string(pid) / "root" / args.file->relative_path();
  support::FileDescriptor artifact(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (artifact.get() < 0) os_error(errno, path.string());
  struct stat info{};
  if (::fstat(artifact.get(), &info) < 0) os_error(errno);
  constexpr std::size_t limit = 4 * 1024 * 1024;
  if (!S_ISREG(info.st_mode) || info.st_size > static_cast<off_t>(limit)) throw support::ToolError("diagnostic artifact must be a regular file no larger than 4 MiB", 2);
  std::string data(limit + 1, '\0');
  const auto [size, error] = support::read_into(artifact.get(), std::span<char>(data));
  if (error) os_error(error);
  if (size > limit) throw support::ToolError("diagnostic artifact grew beyond 4 MiB", 2);
  data.resize(size);
  std::cout << "artifact: " << args.file->string() << ", bytes=" << size << '\n' << repr(data, true) << '\n';
  return 0;
 }
 if (args.action == Action::terminate) {
  if (::syscall(SYS_pidfd_send_signal, process.get(), SIGTERM, nullptr, 0) < 0) os_error(errno);
  return 0;
 }
 return backtrace(pid);
}
}  // namespace
export int mmltk_tool_diagnose_native_process(int argc, char** argv) {
 const auto program = fs::path(argv[0]).filename().string();
 try {
  const auto arguments = parse(argc, argv, program);
  return arguments ? diagnose(*arguments) : 0;
 } catch (const support::ToolError& error) {
  if (error.status == 2) {
   usage(std::cerr, program);
   std::cerr << program << ": error: " << error.what() << '\n';
  } else
   std::cerr << "native process diagnostic failed: " << error.what() << '\n';
  return error.status;
 } catch (const fs::filesystem_error& error) {
  std::cerr << "native process diagnostic failed: [Errno " << error.code().value() << "] " << error.code().message();
  if (!error.path1().empty()) std::cerr << ": " << repr(error.path1().string());
  std::cerr << '\n';
  return 1;
 } catch (const std::exception& error) {
  std::cerr << "native process diagnostic failed: " << error.what() << '\n';
  return 1;
 }
}
