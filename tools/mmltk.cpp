// Native Linux host counterpart of ./mmltk. Container-side shell programs and
// existing repository tools remain their own execution boundaries.
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
import std;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.container_commands;
import mmltk.tools.tool_command;
import mmltk.tools.tool_dispatch;
import mmltk.tools.runtime_package;
namespace {
namespace fs = std::filesystem;
namespace support = mmltk::tools;
namespace commands = mmltk::container_commands;
namespace native = mmltk::tools::command;
namespace package = mmltk::tools::runtime_package;
using native::Command;
using Args = std::vector<std::string>;
std::string env(std::string_view name, std::string_view fallback = {}) {
 const auto value = support::env(name);
 return value.empty() ? std::string(fallback) : value;
}
using support::read_text;
using support::shell_quote;
using support::trim;
using support::write_text;
struct Exit {
 int status;
};
struct Signal {
 int number;
};
volatile sig_atomic_t pending_signal = 0;
volatile sig_atomic_t force_signal = 0;
volatile sig_atomic_t cancelling = 0;
void on_signal(int number) {
 if (cancelling || pending_signal) {
  force_signal = number;
 } else
  pending_signal = number;
}
void install_signals() {
 struct sigaction action{};
 action.sa_handler = on_signal;
 sigemptyset(&action.sa_mask);
 for (int number : {SIGHUP, SIGINT, SIGQUIT, SIGABRT, SIGPIPE, SIGTERM, SIGUSR1, SIGUSR2}) sigaction(number, &action, nullptr);
}
void interrupted() {
 if (pending_signal && !cancelling) throw Signal{pending_signal};
}
void log(const std::string& message) { std::cerr << "mmltk: " << message << '\n'; }
[[noreturn]] void die(const std::string& message) {
 log(message);
 throw Exit{1};
}
void require(bool condition, const std::string& message) {
 if (!condition) die(message);
}
bool matches(const std::string& text, const std::string& pattern) { return std::regex_match(text, std::regex(pattern)); }
bool starts(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }
bool within(const std::string& path, const std::string& root, bool same = true) { return (same && path == root) || starts(path, root + "/"); }
std::string lower(std::string text) {
 for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
 return text;
}
std::string filename(const std::string& path) { return fs::path(path).filename().string(); }
std::string parent(const std::string& path) { return fs::path(path).parent_path().string(); }
void append(Args& to, const Args& from) { to.insert(to.end(), from.begin(), from.end()); }
Args tail(const Args& arguments, std::size_t start = 1) { return Args(arguments.begin() + static_cast<std::ptrdiff_t>(std::min(start, arguments.size())), arguments.end()); }
std::string first(const Args& arguments) { return arguments.empty() ? std::string{} : arguments.front(); }
std::string joined(const Args& arguments, std::string_view delimiter = " ") {
 std::string result;
 bool first_argument = true;
 for (const auto& arg : arguments) {
  if (!first_argument) result += delimiter;
  result += arg;
  first_argument = false;
 }
 return result;
}
std::string quoted(const Args& arguments) {
 Args values;
 for (const auto& value : arguments) values.push_back(shell_quote(value));
 return joined(values);
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
 for (std::size_t at = 0; (at = value.find(from, at)) != std::string::npos; at += to.size()) value.replace(at, from.size(), to);
 return value;
}
Args words(std::string_view text) {
 std::istringstream stream{std::string(text)};
 Args result;
 for (std::string value; stream >> value;) result.push_back(value);
 return result;
}
Args split_zero(std::string_view text) { return support::split(text, '\0'); }
bool present(const std::string& path) {
 std::error_code error;
 return fs::exists(path, error);
}
bool symlink(const std::string& path) {
 std::error_code error;
 return fs::is_symlink(fs::symlink_status(path, error));
}
bool executable(const std::string& path) { return access(path.c_str(), X_OK) == 0; }
bool command_exists(const std::string& name) {
 if (name.find('/') != std::string::npos) return executable(name);
 for (const auto& dir : support::split(env("PATH"), ':'))
  if (executable((dir.empty() ? "." : dir) + "/" + name)) return true;
 return false;
}
std::string read_optional(const std::string& path) {
 try {
  return read_text(path);
 } catch (...) { return {}; }
}
std::string start_ticks(pid_t pid, pid_t expected_parent = -1) {
 const auto text = read_optional("/proc/" + std::to_string(pid) + "/stat");
 const auto end = text.rfind(") ");
 if (end == std::string::npos) return {};
 const auto fields = words(std::string_view(text).substr(end + 2));
 if (fields.size() < 20 || fields[0] == "Z" || (expected_parent >= 0 && fields[1] != std::to_string(expected_parent))) return {};
 return fields[19];
}
struct Fd {
 int value = -1;
 Fd() = default;
 explicit Fd(int fd) : value(fd) {
  if (fd < 0) throw std::runtime_error(std::strerror(errno));
 }
 Fd(const Fd&) = delete;
 Fd& operator=(const Fd&) = delete;
 Fd(Fd&& other) noexcept : value(std::exchange(other.value, -1)) {}
 Fd& operator=(Fd&& other) noexcept {
  if (this != &other) {
   reset();
   value = std::exchange(other.value, -1);
  }
  return *this;
 }
 ~Fd() { reset(); }
 void reset() {
  if (value >= 0) close(std::exchange(value, -1));
 }
};
struct ContainerUser {
 uid_t uid = getuid();
 gid_t gid = getgid();
 std::string target;
};
template <class Capture>
ContainerUser resolve_container_user(Capture&& capture) {
 ContainerUser selected;
 if (selected.uid != 0) return selected;
 selected.target = env("MMLTK_USER");
 if (!selected.target.empty()) {
  selected.uid = static_cast<uid_t>(std::stoul(capture(Args{"id", "-u", selected.target})));
  selected.gid = static_cast<gid_t>(std::stoul(capture(Args{"id", "-g", selected.target})));
 } else {
  for (const auto& line : support::lines(read_text("/etc/passwd"))) {
   const auto fields = support::split(line, ':');
   if (fields.size() < 7) continue;
   if (std::stoul(fields[2]) >= 1000 && !fields[6].ends_with("/nologin") && !fields[6].ends_with("/false")) {
    selected.target = fields[0];
    selected.uid = static_cast<uid_t>(std::stoul(fields[2]));
    selected.gid = static_cast<gid_t>(std::stoul(fields[3]));
    break;
   }
  }
 }
 return selected;
}
std::optional<ContainerUser> consume_bootstrap_handoff() {
 const auto descriptor = env("MMLTK_BOOTSTRAP_LOCK_FD");
 const auto checkout = env("MMLTK_BOOTSTRAP_CHECKOUT");
 const auto cache = env("MMLTK_BOOTSTRAP_CACHE");
 const auto stage = env("MMLTK_BOOTSTRAP_STAGE");
 for (const auto* name : {"MMLTK_BOOTSTRAP_LOCK_FD", "MMLTK_BOOTSTRAP_CHECKOUT", "MMLTK_BOOTSTRAP_CACHE", "MMLTK_BOOTSTRAP_STAGE"}) unsetenv(name);
 if (descriptor.empty()) return {};
 require(matches(descriptor, "[0-9]+"), "invalid bootstrap lock descriptor");
 const int raw = std::stoi(descriptor);
 require(raw > STDERR_FILENO, "invalid bootstrap lock descriptor");
 Fd lock(raw);
 require(fcntl(lock.value, F_SETFD, FD_CLOEXEC) == 0, "cannot adopt bootstrap lock descriptor");
 require(!checkout.empty() && fs::canonical(checkout) == fs::path(checkout) && within(cache, checkout + "/.cache") && fs::weakly_canonical(cache) == fs::path(cache) &&
          parent(stage) == checkout + "/build/.tools-generations" && fs::is_directory(stage),
  "invalid bootstrap tools paths");
 const auto lock_path = checkout + "/.cache/locks/tools.lock";
 struct stat inherited{}, expected{};
 require(fstat(lock.value, &inherited) == 0 && stat(lock_path.c_str(), &expected) == 0 && S_ISREG(inherited.st_mode) && inherited.st_dev == expected.st_dev && inherited.st_ino == expected.st_ino &&
          flock(lock.value, LOCK_EX | LOCK_NB) == 0,
  "invalid bootstrap tools lock");
 const auto selected = resolve_container_user([](const Args& args) {
  support::RunOptions options;
  options.capture_stdout = true;
  const auto result = support::run(args, options);
  if (result.exit_code) throw Exit{result.exit_code};
  return mmltk::tools::text::trim_newlines(result.out);
 });
 auto own = [&](const fs::path& path) {
  struct stat metadata{};
  require(lstat(path.c_str(), &metadata) == 0, "cannot inspect bootstrap tools path '" + path.string() + "'");
  if ((metadata.st_uid != selected.uid || metadata.st_gid != selected.gid) && lchown(path.c_str(), selected.uid, selected.gid) != 0)
   die("cannot own bootstrap tools path '" + path.string() + "': " + std::strerror(errno));
 };
 for (auto path = fs::path(cache) / "cmake"; within(path.string(), checkout + "/.cache"); path = path.parent_path()) own(path);
 for (const auto& path : {checkout + "/.cache/locks", lock_path, checkout + "/build", checkout + "/build/.tools-generations", checkout + "/build/tools", checkout + "/mmltk"}) own(path);
 require(fchmod(lock.value, 0664) == 0, "cannot set bootstrap tools lock permissions");
 for (const auto& tree : {cache + "/cmake/tools", stage}) {
  require(fs::canonical(tree) == fs::path(tree), "bootstrap tools tree must remain inside its owned path: " + tree);
  own(tree);
  for (const auto& entry : fs::recursive_directory_iterator(tree)) own(entry.path());
 }
 // Closing the inherited open-file description releases admission before the
 // wrapper takes checkout/cache locks; no child inherits the descriptor.
 return selected;
}
Fd anonymous_file() {
 std::string pattern = env("TMPDIR", "/tmp") + "/mmltk-native.XXXXXX";
 int fd = mkstemp(pattern.data());
 if (fd < 0) throw std::runtime_error("cannot create temporary file: " + std::string(std::strerror(errno)));
 unlink(pattern.c_str());
 fcntl(fd, F_SETFD, FD_CLOEXEC);
 return Fd(fd);
}
template <class Operation>
ssize_t checked_io(Operation&& operation) {
 for (;;) {
  const auto count = operation();
  if (count >= 0) return count;
  if (errno != EINTR) throw std::runtime_error(std::strerror(errno));
  interrupted();
 }
}
void reap(pid_t child) {
 while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
}
void write_fd(int fd, std::string_view text) {
 while (!text.empty()) {
  const auto count = checked_io([&] { return write(fd, text.data(), text.size()); });
  text.remove_prefix(static_cast<std::size_t>(count));
 }
}
std::string read_fd(int fd) {
 lseek(fd, 0, SEEK_SET);
 std::string result;
 std::array<char, 65536> buffer{};
 for (;;) {
  const auto count = checked_io([&] { return read(fd, buffer.data(), buffer.size()); });
  if (count == 0) return result;
  result.append(buffer.data(), static_cast<std::size_t>(count));
 }
}
int exit_status(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1; }
struct Result {
 int status = 0;
 std::string out;
};
struct Run {
 bool capture = false, quiet = false, stderr_quiet = false, merge = false, to_stderr = false;
 std::optional<std::string> input{};
 std::string input_file{};
 std::map<std::string, std::string> environment{};
};
struct Child {
 pid_t pid;
 std::string start;
};
class Wrapper {
 std::string repo, host_workdir, self, cache, repository_cache, buildkit, cmake, cargo, ccache, firefox, firefox_input, fingerprints, locks, release_stage;
 std::string container_cache, container_stage, sccache_basedirs;
 std::string tools_image;
 std::string image_name, validation_image, build_image, toolchain_image, gcc_image, payload_image, runtime_base, ubuntu_image, ort_image, dependencies_image, analysis_image;
 std::string base_name, container_name, release_container, package_container, gui_container, test_build_container, test_run_container, tidy_container, cleanup_container, builder;
 std::string donor, cpu_list, mode = "initializing", wrapper_start;
 pid_t wrapper_pid = getpid();
 uid_t uid = getuid();
 gid_t gid = getgid();
 long jobs = 1;
 bool quiet_gui = false, producers_prepared = false, builder_ready = false, default_builder_ready = false, stale_cleaned = false;
 bool cleanup_armed = false, headless_active = false, trace_enabled = false;
 bool gui = false, build = false, build_gui = false, protocol = false, bindings = false, export_shiftlut = false, update_gui = false, update_firefox = false;
 bool build_tools = false, tidy_tools = false;
 bool tidy = false, third_party = false, prepare_gui = false, test = false, declaration = false, cleanup_report = false, diagnose_io = false, diagnose_nvidia = false, diagnose_gpu = false;
 bool test_config_explicit = false, debugger = false, headless = false;
 std::string shiftlut_source, shiftlut_operation, declaration_operation, cleanup_profile, io_path, gpu_image = "runtime", suite, test_config = "release", test_executable;
 Args original, passthrough, rewritten, test_args, test_environment, debugger_commands, declaration_args, nvidia_args, nvidia_configuration;
 Args mounts, groups, execution_args, container_command, active_container_names;
 std::string gui_platform, gui_runtime = "/tmp/mmltk-gui-runtime", wayland_host, wayland_container, dbus_host, dbus_container, runtime_label;
 std::string trace_file, ccache_stats, sccache_stats, container_trace, container_ccache_stats, container_sccache_stats, firefox_trace, container_firefox_trace;
 std::string operation_log, test_registration, runtime_registration;
 // Tools admission survives exceptional child exits until finish settles the
 // owned container; a stack-local lock would unlock before cancellation.
 Fd checkout_lock, cache_lock, runtime_lock, tools_stage_lock, saved_stdout, saved_stderr;
 std::vector<pid_t> log_pids;
 std::vector<Child> processes, build_children;
 std::vector<std::pair<std::string, std::string>> daemon_mounts;
 bool mount_mapping_ready = false;
 pid_t active_docker = 0;
 bool done = false;
 static Args tool_arguments(Command selected, const Args& arguments = {}, std::string_view executable = "/workspace/build/tools/mmltk") {
  return native::self_invocation(selected, arguments, {}, executable);
 }
 static std::string tool_shell(Command selected) { return native::shell_invocation(selected, {}, {}, "/workspace/build/tools/mmltk"); }
 void diagnostic_tools(Args& args) { append(args, {"--volume", repo + "/build/tools:/opt/mmltk-tools:ro"}); }
 Result run(const Args& args, Run options = {}) {
  interrupted();
  require(!args.empty(), "empty wrapper process invocation");
  auto argv = support::detail::argument_pointers(args);
  Fd output, input, null;
  if (options.capture) output = anonymous_file();
  if (options.input) {
   input = anonymous_file();
   write_fd(input.value, *options.input);
   lseek(input.value, 0, SEEK_SET);
  } else if (!options.input_file.empty())
   input = Fd(open(options.input_file.c_str(), O_RDONLY | O_CLOEXEC));
  if (options.quiet || options.stderr_quiet) null = Fd(open("/dev/null", O_RDWR | O_CLOEXEC));
  std::cout.flush();
  std::cerr.flush();
  const pid_t pid = fork();
  if (pid < 0) die("cannot start process: " + std::string(std::strerror(errno)));
  if (pid == 0) {
   for (int number : {SIGHUP, SIGINT, SIGQUIT, SIGABRT, SIGPIPE, SIGTERM, SIGUSR1, SIGUSR2}) signal(number, SIG_DFL);
   if (input.value >= 0) dup2(input.value, STDIN_FILENO);
   if (options.capture)
    dup2(output.value, STDOUT_FILENO);
   else if (options.quiet)
    dup2(null.value, STDOUT_FILENO);
   else if (options.to_stderr)
    dup2(STDERR_FILENO, STDOUT_FILENO);
   if (options.merge)
    dup2(STDOUT_FILENO, STDERR_FILENO);
   else if (options.quiet || options.stderr_quiet)
    dup2(null.value, STDERR_FILENO);
   for (const auto& [name, value] : options.environment) setenv(name.c_str(), value.c_str(), 1);
   execvp(argv[0], argv.data());
   const int error = errno;
   const std::string message = "mmltk: " + args[0] + ": " + std::strerror(error) + "\n";
   write(STDERR_FILENO, message.data(), message.size());
   _exit(error == ENOENT ? 127 : 126);
  }
  processes.push_back({pid, start_ticks(pid, getpid())});
  if (!cancelling && cleanup_armed && (mode == "gui" || mode == "raw") && args.size() > 1 && args[0] == "docker" && args[1] == "exec") active_docker = pid;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
   if (errno == EINTR) {
    if (cancelling && force_signal) stop_process(pid, start_ticks(pid, getpid()), getpid());
    interrupted();
    continue;
   }
   if (errno != ECHILD) die("wait failed: " + std::string(std::strerror(errno)));
   status = 1 << 8;
   break;
  }
  if (active_docker == pid) active_docker = 0;
  std::erase_if(processes, [pid](const Child& item) { return item.pid == pid; });
  interrupted();
  return {exit_status(status), options.capture ? read_fd(output.value) : std::string{}};
 }
 void must(const Args& args, Run options = {}) {
  const auto result = run(args, std::move(options));
  if (result.status) throw Exit{result.status};
 }
 Result capture(const Args& args, bool hide_error = false) { return run(args, {.capture = true, .stderr_quiet = hide_error}); }
 std::string get(const Args& args, bool hide_error = false) {
  const auto result = capture(args, hide_error);
  if (result.status) throw Exit{result.status};
  return mmltk::tools::text::trim_newlines(result.out);
 }
 std::string normalized(const fs::path& path) { return get({"realpath", "-m", "--", path.string()}); }
 std::string existing(const fs::path& path) { return get({"realpath", "-e", "--", path.string()}); }
 bool ok(const Args& args) { return run(args, {.quiet = true}).status == 0; }
 void resolve_daemon_mounts() {
  if (mount_mapping_ready) return;
  mount_mapping_ready = true;
  if (!present("/.dockerenv") || env("MMLTK_DOCKER_CONTAINER") == "host") return;
  const auto container = env("MMLTK_DOCKER_CONTAINER", env("HOSTNAME"));
  const auto inspection = run({"timeout", "--signal=KILL", "15s", "docker", "inspect", "--format", "{{json .Mounts}}", container}, {.capture = true, .stderr_quiet = true});
  require(!inspection.status, "cannot resolve invoking container mount mapping");
  const auto entries = support::json::parse(inspection.out);
  for (const auto& entry : entries) daemon_mounts.emplace_back(entry.at("Destination").get<std::string>(), entry.at("Source").get<std::string>());
  std::sort(daemon_mounts.begin(), daemon_mounts.end(), [](const auto& left, const auto& right) { return left.first.size() > right.first.size(); });
  require(!daemon_mounts.empty(), "invoking container has no daemon-visible bind mounts");
 }
 std::string daemon_path(const std::string& path) {
  for (const auto& [destination, source] : daemon_mounts)
   if (within(path, destination)) return source + path.substr(destination.size());
  require(daemon_mounts.empty() || path == "/" || path == "/dev" || starts(path, "/dev/"), "path is not visible through invoking container mounts: " + path);
  return path;
 }
 Args docker_args(Args args) {
  Args mapped;
  bool container_options = !args.empty() && args.front() == "run";
  for (std::size_t index = 0; index < args.size(); ++index) {
   mapped.push_back(args[index]);
   if (!container_options || index == 0) continue;
   if (!starts(args[index], "-")) {
    container_options = false;
    continue;
   }
   if (args[index] != "--volume") {
    const auto& option = args[index];
    const bool flag =
     option == "--rm" || option == "--init" || option == "--privileged" || option == "--read-only" || option == "-d" || option == "-i" || option == "-t" || option.find('=') != std::string::npos;
    if (!flag && index + 1 < args.size()) mapped.push_back(args[++index]);
    continue;
   }
   require(index + 1 < args.size(), "missing bind mount specification");
   resolve_daemon_mounts();
   const auto specification = args[++index];
   const auto separator = specification.find(':');
   require(separator != std::string::npos, "invalid bind mount specification");
   const auto source = specification.substr(0, separator);
   mapped.push_back(daemon_path(source) + specification.substr(separator));
   if (source == "/" && !daemon_mounts.empty()) {
    const auto destination_end = specification.find(':', separator + 1);
    const auto destination = specification.substr(separator + 1, destination_end - separator - 1);
    const auto mode = destination_end == std::string::npos ? ":rw" : specification.substr(destination_end);
    for (const auto& [local, host] : daemon_mounts) append(mapped, {"--volume", host + ":" + destination + local + mode});
   }
  }
  mapped.insert(mapped.begin(), "docker");
  return mapped;
 }
 Result docker(Args args, Run options = {}) { return run(docker_args(std::move(args)), std::move(options)); }
 void docker_must(Args args, Run options = {}) { must(docker_args(std::move(args)), std::move(options)); }
 std::string docker_get(Args args, bool hide_error = false) { return get(docker_args(std::move(args)), hide_error); }
 bool docker_ok(Args args) { return ok(docker_args(std::move(args))); }
 Args timeout(Args args, int seconds, bool foreground = true) {
  Args result{"timeout"};
  if (foreground) result.push_back("--foreground");
  append(result, {"--signal=KILL", std::to_string(seconds) + "s"});
  append(result, args);
  return result;
 }
 std::string sha(std::string data) {
  auto result = run({"sha256sum"}, {.capture = true, .input = std::move(data)});
  if (result.status) throw Exit{result.status};
  return result.out.substr(0, 64);
 }
 Args sorted_paths(Args paths) {
  std::string input;
  for (const auto& path : paths) {
   input += path;
   input.push_back('\0');
  }
  const auto result = run({"sort", "-z"}, {.capture = true, .input = std::move(input)});
  if (result.status) throw Exit{result.status};
  auto sorted = split_zero(result.out);
  if (!sorted.empty() && sorted.back().empty()) sorted.pop_back();
  return sorted;
 }
 std::string sha_files(const Args& paths) {
  Args command{"sha256sum"};
  append(command, paths);
  auto r = capture(command);
  if (r.status) throw Exit{r.status};
  return r.out;
 }
 std::string fingerprint(const Args& values, const Args& relative_files) {
  std::string data;
  for (const auto& value : values) data += value + "\n";
  Args paths;
  for (const auto& path : relative_files) paths.push_back(repo + "/" + path);
  if (!paths.empty()) data += sha_files(paths);
  return sha(std::move(data));
 }
 void quiet_log(const std::string& text) {
  if (!quiet_gui) log(text);
 }
 std::string user() const { return std::to_string(uid) + ":" + std::to_string(gid); }
 void owned_dirs(const Args& paths) {
  for (const auto& path : paths) {
   must({"install", "-d", "-m", "0755", path});
   chown(path.c_str(), uid, gid);
  }
 }
 void cache_layout() {
  owned_dirs(
   {cache, cmake + "/release", cargo + "/home", cargo + "/target/browser-app", ccache, firefox + "/obj-minimal-opt", firefox + "/mozbuild", firefox + "/sccache", buildkit, fingerprints, locks});
 }
 void compiler_cache_dirs() {
  owned_dirs(
   {cache + "/container-home", cmake, cargo + "/home", cargo + "/target/browser-app", ccache, ccache + "/tmp", firefox + "/obj-minimal-opt", firefox + "/mozbuild", firefox + "/sccache",
    parent(release_stage), repo + "/build"});
 }
 void labels(Args& args, const std::string& label_mode) {
  append(args, {"--label", "com.mmltk.wrapper=1", "--label", "com.mmltk.repo=" + repo, "--label", "com.mmltk.owner-pid=" + std::to_string(wrapper_pid), "--label",
                "com.mmltk.owner-start=" + wrapper_start, "--label", "com.mmltk.mode=" + label_mode});
 }
 void prepare_lock(const std::string& path) {
  owned_dirs({parent(path)});
  Fd fd(open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0664));
  chown(path.c_str(), uid, gid);
  chmod(path.c_str(), 0664);
  require(access(path.c_str(), R_OK | W_OK) == 0, "lock file '" + path + "' is not accessible to uid " + std::to_string(uid));
 }
 Fd lock_file(const std::string& path, int flags, const std::string& error = {}) {
  prepare_lock(path);
  Fd fd(open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
  interrupted();
  while (flock(fd.value, flags) != 0) {
   if (errno == EINTR) {
    interrupted();
    continue;
   }
   die(error.empty() ? "cannot lock '" + path + "'" : error);
  }
  interrupted();
  return fd;
 }
 void acquire_mutation() {
  if (checkout_lock.value >= 0) return;
  require(command_exists("flock"), "flock is required");
  prepare_lock(repository_cache + "/locks/runtime.lock");
  checkout_lock = lock_file(repository_cache + "/locks/checkout.lock", LOCK_EX | LOCK_NB, "another build, GUI build, test, or tidy invocation is already active in this checkout");
  cache_lock = lock_file(locks + "/cache-root.lock", LOCK_EX | LOCK_NB, "another invocation is already mutating cache root '" + cache + "'");
 }
 void release_mutation() {
  if (cache_lock.value >= 0) flock(cache_lock.value, LOCK_UN);
  cache_lock.reset();
  if (checkout_lock.value >= 0) flock(checkout_lock.value, LOCK_UN);
  checkout_lock.reset();
 }
 void release_runtime() {
  if (runtime_lock.value >= 0) flock(runtime_lock.value, LOCK_UN);
  runtime_lock.reset();
 }
 bool wait_pid(pid_t pid, int milliseconds) {
  const auto identity = start_ticks(pid);
  if (identity.empty()) return true;
  if (cancelling && force_signal && pid == active_docker) return false;
  Fd fd;
  const int raw_fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
  if (raw_fd < 0) {
   if (errno == ESRCH) return true;
   const auto result = run({"timeout", "--foreground", "--signal=KILL", std::to_string(milliseconds / 1000.0) + "s", "pidwait", "-F", "/dev/stdin"}, {.input = std::to_string(pid) + "\n"});
   return !result.status || start_ticks(pid) != identity;
  }
  fd = Fd(raw_fd);
  pollfd item{fd.value, POLLIN, 0};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  for (;;) {
   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
   const int result = poll(&item, 1, static_cast<int>(std::max<std::int64_t>(0, remaining)));
   if (result >= 0) return result > 0 || start_ticks(pid) != identity;
   if (errno != EINTR) return start_ticks(pid) != identity;
   if (cancelling && force_signal && pid == active_docker) return false;
   interrupted();
  }
 }
 void stop_process(pid_t pid, const std::string& expected, pid_t expected_parent = -1) {
  if (expected.empty() || start_ticks(pid, expected_parent) != expected) return;
  if (kill(pid, SIGSTOP)) return;
  for (const auto& child : words(read_optional("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children"))) {
   const pid_t child_pid = static_cast<pid_t>(std::stol(child));
   stop_process(child_pid, start_ticks(child_pid, pid), pid);
  }
  if (start_ticks(pid, expected_parent) == expected) kill(pid, SIGKILL);
 }
 void terminate_builds() {
  for (const auto& child : build_children) stop_process(child.pid, child.start, getpid());
  std::vector<Child> owners{{wrapper_pid, wrapper_start}};
  owners.insert(owners.end(), build_children.begin(), build_children.end());
  for (const auto& owner : owners) {
   if (owner.start.empty() || (owner.pid == wrapper_pid && active_container_names.empty())) continue;
   Args args{"docker", "ps", "-aq", "--filter", "label=com.mmltk.owner-pid=" + std::to_string(owner.pid), "--filter", "label=com.mmltk.owner-start=" + owner.start};
   if (owner.pid == wrapper_pid)
    for (const auto& name : active_container_names) append(args, {"--filter", "name=^/" + name + "$"});
   const auto ids = run(timeout(args, 5, false), {.capture = true, .stderr_quiet = true});
   for (const auto& id : words(ids.out)) run(timeout({"docker", "rm", "-f", id}, 5, false), {.quiet = true});
  }
  for (const auto& child : build_children) { reap(child.pid); }
  build_children.clear();
  active_container_names.clear();
 }
 template <class Operation>
 Result owned_container(const std::string& name, Operation&& operation) {
  active_container_names = {name};
  docker({"rm", "-f", name}, {.quiet = true});
  const auto result = operation();
  // Keep registration on exceptional exits so wrapper shutdown can remove it.
  active_container_names.clear();
  return result;
 }
 Result owned_docker(const std::string& name, Args args, Run options = {}) {
  return owned_container(name, [&] { return docker(std::move(args), std::move(options)); });
 }
 void owned_docker_must(const std::string& name, Args args, Run options = {}) {
  auto r = owned_docker(name, std::move(args), std::move(options));
  if (r.status) throw Exit{r.status};
 }
 void task(std::function<void()> body) {
  std::cout.flush();
  std::cerr.flush();
  const auto pid = fork();
  require(pid >= 0, "cannot start parallel build worker");
  if (!pid) {
   processes.clear();
   build_children.clear();
   active_container_names.clear();
   pending_signal = 0;
   force_signal = 0;
   int result = 0;
   try {
    body();
   } catch (const Exit& error) { result = error.status; } catch (const Signal& signal) {
    result = cancel(signal.number);
   } catch (const std::exception& error) {
    log(error.what());
    result = 1;
   }
   std::cout.flush();
   std::cerr.flush();
   _exit(result);
  }
  build_children.push_back({pid, start_ticks(pid, getpid())});
 }
 void wait_tasks() {
  std::set<pid_t> remaining;
  for (const auto& child : build_children) remaining.insert(child.pid);
  while (!remaining.empty()) {
   int status;
   const auto pid = waitpid(-1, &status, 0);
   if (pid < 0) {
    if (errno == EINTR) {
     interrupted();
     continue;
    }
    die("parallel build wait failed");
   }
   if (!remaining.erase(pid)) continue;
   const int result = exit_status(status);
   if (result) {
    log("parallel build job failed with exit status " + std::to_string(result) + "; stopping its peer");
    terminate_builds();
    throw Exit{result};
   }
  }
  build_children.clear();
  active_container_names.clear();
 }
 int wait_one_task() {
  require(build_children.size() == 1, "expected one owned worker");
  int status = 0;
  while (waitpid(build_children[0].pid, &status, 0) < 0) {
   if (errno == EINTR) {
    interrupted();
    continue;
   }
   die("owned worker wait failed");
  }
  build_children.clear();
  active_container_names.clear();
  return exit_status(status);
 }
 void prepare_logging(std::string path) {
  if (path.empty()) {
   const auto now = std::time(nullptr);
   std::tm local{};
   localtime_r(&now, &local);
   std::ostringstream stamp;
   stamp << std::put_time(&local, "%Y%m%d-%H%M%S");
   path = repo + "/build/logs/mmltk-" + mode + "-" + stamp.str() + "-" + std::to_string(wrapper_pid) + ".log";
  } else if (!starts(path, "/"))
   path = host_workdir + "/" + path;
  fs::create_directories(parent(path));
  Fd file(open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0666));
  chown(parent(path).c_str(), uid, gid);
  chown(path.c_str(), uid, gid);
  std::cout.flush();
  std::cerr.flush();
  saved_stdout = Fd(fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3));
  saved_stderr = Fd(fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3));
  for (const int stream : {STDOUT_FILENO, STDERR_FILENO}) {
   int descriptors[2];
   require(pipe2(descriptors, O_CLOEXEC) == 0, "cannot create transcript pipe");
   Fd reader(descriptors[0]), writer(descriptors[1]);
   const auto pid = fork();
   require(pid >= 0, "cannot start transcript capture");
   if (pid == 0) {
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    dup2(reader.value, STDIN_FILENO);
    dup2(stream == STDOUT_FILENO ? saved_stdout.value : saved_stderr.value, STDOUT_FILENO);
    dup2(saved_stderr.value, STDERR_FILENO);
    execlp("tee", "tee", "--output-error=warn", "-a", path.c_str(), static_cast<char*>(nullptr));
    _exit(127);
   }
   log_pids.push_back(pid);
   dup2(writer.value, stream);
  }
  operation_log = std::move(path);
  log("writing " + mode + " log to " + operation_log);
 }
 void append_trace(const support::json& record) {
  Fd fd(open(trace_file.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
  write_fd(fd.value, record.dump() + "\n");
 }
 void trace_event(const std::string& phase, const std::string& event, const std::string& result = {}) {
  if (!trace_enabled) return;
  const auto timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  support::json record{{"timestamp_ns", timestamp}, {"pid", getpid()}, {"phase", phase}, {"event", event}, {"result", result}};
  append_trace(record);
 }
 void traced(const std::string& phase, const std::function<void()>& body) {
  trace_event(phase, "start");
  try {
   body();
   trace_event(phase, "finish", "success");
  } catch (const Exit& failure) {
   trace_event(phase, "finish", "failure:" + std::to_string(failure.status));
   throw;
  } catch (const Signal& signal) {
   trace_event(phase, "finish", "failure:" + std::to_string(128 + signal.number));
   throw;
  } catch (...) {
   trace_event(phase, "finish", "failure:1");
   throw;
  }
 }
 void initialize_trace() {
  if (trace_file.empty()) return;
  if (!starts(trace_file, "/")) trace_file = host_workdir + "/" + trace_file;
  trace_file = normalized(trace_file);
  require(within(trace_file, repo, false), "MMLTK_BUILD_TRACE_FILE must stay under " + repo);
  owned_dirs({parent(trace_file)});
  { Fd file(open(trace_file.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0666)); }
  chown(trace_file.c_str(), uid, gid);
  container_trace = "/workspace" + trace_file.substr(repo.size());
  firefox_trace = trace_file;
  if (firefox_trace.ends_with(".jsonl")) firefox_trace.resize(firefox_trace.size() - 6);
  firefox_trace += "-firefox-stage.jsonl";
  write_text(firefox_trace, "");
  chown(firefox_trace.c_str(), uid, gid);
  container_firefox_trace = "/workspace" + firefox_trace.substr(repo.size());
  for (auto [path, container_path, name] :
   {std::tuple{&ccache_stats, &container_ccache_stats, "MMLTK_CCACHE_STATS_FILE"}, std::tuple{&sccache_stats, &container_sccache_stats, "MMLTK_FIREFOX_SCCACHE_STATS_FILE"}}) {
   if (path->empty()) continue;
   *path = normalized(starts(*path, "/") ? *path : host_workdir + "/" + *path);
   require(within(*path, repo + "/build", false), std::string(name) + " must stay under " + repo + "/build");
   owned_dirs({parent(*path)});
   write_text(*path, "");
   chown(path->c_str(), uid, gid);
   *container_path = "/workspace" + path->substr(repo.size());
  }
  trace_enabled = true;
 }
 void stop_previous(const std::string& kind) {
  const Args modes = kind == "test" ? Args{"test"} : Args{"gui", "raw"};
  require(command_exists("docker"), "docker is not installed");
  Args containers, registrations;
  std::map<pid_t, std::string> owners;
  for (const auto& old_mode : modes) {
   auto result = capture(timeout({"docker", "ps", "-a", "--filter", "label=com.mmltk.wrapper=1", "--filter", "label=com.mmltk.repo=" + repo, "--filter", "label=com.mmltk.mode=" + old_mode, "--format",
                                  "{{.ID}} {{.Label \"com.mmltk.owner-pid\"}} {{.Label \"com.mmltk.owner-start\"}}"},
    15));
   require(!result.status, "could not enumerate previous " + kind + " containers");
   for (const auto& line : support::lines(result.out)) {
    const auto fields = words(line);
    if (fields.empty() || (fields.size() > 1 && fields[1] == std::to_string(wrapper_pid))) continue;
    containers.push_back(fields[0]);
    if (fields.size() >= 3 && matches(fields[1], "[1-9][0-9]*") && matches(fields[2], "[0-9]+")) owners[static_cast<pid_t>(std::stol(fields[1]))] = fields[2];
   }
  }
  const std::string directory = repository_cache + "/locks";
  if (present(directory))
   for (const auto& entry : fs::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file() || !starts(name, kind + "-run-") || !name.ends_with(".owner")) continue;
    registrations.push_back(entry.path().string());
    const auto fields = words(read_optional(entry.path().string()));
    if (fields.size() == 2 && matches(fields[0], "[1-9][0-9]*") && matches(fields[1], "[0-9]+") && fields[0] != std::to_string(wrapper_pid))
     owners[static_cast<pid_t>(std::stol(fields[0]))] = fields[1];
   }
  if (kind == "runtime")
   for (const auto& [pid, expected] : owners) {
    const auto live = start_ticks(pid);
    if (live == expected && std::stoull(live) > std::stoull(wrapper_start)) die("GUI launch was superseded by a newer runtime invocation");
   }
  for (const auto& [pid, expected] : owners)
   if (start_ticks(pid) == expected) {
    if (kind == "test" || !quiet_gui) log("stopping previous " + kind + " run pid=" + std::to_string(pid));
    kill(pid, SIGTERM);
   }
  if (kind == "runtime")
   for (const auto& [pid, expected] : owners)
    if (start_ticks(pid) == expected) wait_pid(pid, 15000);
  for (const auto& id : containers) run(timeout({"docker", "stop", "--time", "22", id}, 25), {.quiet = true});
  for (const auto& [pid, expected] : owners)
   if (start_ticks(pid) == expected && !wait_pid(pid, 5000)) {
    if (kind == "test" || !quiet_gui) log("forcing previous " + kind + " run to stop pid=" + std::to_string(pid));
    stop_process(pid, expected);
    require(wait_pid(pid, 5000), "previous " + kind + " wrapper did not stop");
   }
  for (const auto& id : containers) run(timeout({"docker", "rm", "-f", id}, 15), {.quiet = true});
  for (const auto& old_mode : modes) {
   auto result = capture(timeout({"docker", "ps", "-aq", "--filter", "label=com.mmltk.wrapper=1", "--filter", "label=com.mmltk.repo=" + repo, "--filter", "label=com.mmltk.mode=" + old_mode}, 15));
   require(!result.status, "could not verify previous " + kind + " container cleanup");
   require(trim(result.out).empty(), "previous " + kind + " containers remain; replacement did not start");
  }
  for (const auto& registration : registrations) fs::remove(registration);
 }
 void begin_test(const std::string& selected) {
  auto admission = lock_file(repository_cache + "/locks/test-admission.lock", LOCK_EX);
  if (selected == "all") stop_previous("test");
  test_registration = repository_cache + "/locks/test-run-" + std::to_string(wrapper_pid) + ".owner";
  write_text(test_registration, std::to_string(wrapper_pid) + " " + wrapper_start + "\n");
 }
 void acquire_runtime() {
  if (runtime_lock.value >= 0) return;
  require(command_exists("flock"), "flock is required");
  auto admission = lock_file(repository_cache + "/locks/runtime-admission.lock", LOCK_EX);
  const auto path = repository_cache + "/locks/runtime.lock";
  prepare_lock(path);
  runtime_lock = Fd(open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
  if (gui) {
   if (flock(runtime_lock.value, LOCK_EX | LOCK_NB)) {
    require(!prepare_gui, "GUI container preparation cannot start while a runtime invocation is active");
    ensure_docker();
    ensure_image();
    resolve_gui();
    stop_previous("runtime");
    // flock's bounded wait operates on the same open file description.
    const int flags = fcntl(runtime_lock.value, F_GETFD);
    fcntl(runtime_lock.value, F_SETFD, flags & ~FD_CLOEXEC);
    const int status = run({"flock", "-w", "5", "-x", std::to_string(runtime_lock.value)}).status;
    fcntl(runtime_lock.value, F_SETFD, flags);
    require(!status, "previous runtime did not release its lock; replacement GUI did not start");
   }
  } else
   require(flock(runtime_lock.value, LOCK_SH | LOCK_NB) == 0, "raw mmltk invocation cannot start while the GUI is active");
  runtime_registration = repository_cache + "/locks/runtime-run-" + std::to_string(wrapper_pid) + ".owner";
  write_text(runtime_registration, std::to_string(wrapper_pid) + " " + wrapper_start + "\n");
 }
 bool server_ready() { return docker_ok({"version", "--format", "{{.Server.Version}}"}); }
 void stale_cleanup() {
  if (stale_cleaned) return;
  stale_cleaned = true;
  const auto ids = docker({"ps", "-aq", "--filter", "label=com.mmltk.wrapper=1", "--filter", "label=com.mmltk.repo=" + repo}, {.capture = true, .stderr_quiet = true});
  for (const auto& id : words(ids.out)) {
   auto owner = docker({"inspect", "--format", "{{ index .Config.Labels \"com.mmltk.owner-pid\" }}", id}, {.capture = true, .stderr_quiet = true});
   auto identity = docker({"inspect", "--format", "{{ index .Config.Labels \"com.mmltk.owner-start\" }}", id}, {.capture = true, .stderr_quiet = true});
   const auto owner_pid = trim(owner.out);
   const auto expected = trim(identity.out);
   if (matches(owner_pid, "[1-9][0-9]*") && !expected.empty() && start_ticks(static_cast<pid_t>(std::stol(owner_pid))) == expected) continue;
   quiet_log("removing orphaned wrapper container " + id);
   docker({"rm", "-f", id}, {.quiet = true});
  }
 }
 void ensure_docker() {
  require(command_exists("docker"), "docker is not installed");
  if (server_ready()) {
   stale_cleanup();
   return;
  }
  quiet_log("docker daemon is unavailable; attempting a best-effort start");
  for (const auto& attempt : std::vector<Args>{
        {"systemctl", "--user", "start", "docker"}, {"systemctl", "start", "docker"}, {"service", "docker", "start"}, {"sudo", "-n", "systemctl", "--user", "start", "docker"},
        {"sudo", "-n", "systemctl", "start", "docker"}, {"sudo", "-n", "service", "docker", "start"}
       })
   if (ok(attempt)) break;
  require(server_ready(), "docker is still unavailable. Start dockerd, then rerun this command.");
  stale_cleanup();
 }
 void ensure_image() { require(docker_ok({"image", "inspect", image_name}), "image '" + image_name + "' is missing. Build it first with: ./mmltk --build"); }
 std::string image_id(const std::string& image) { return docker_get({"image", "inspect", image, "--format", "{{.Id}}"}); }
 std::string image_label(const std::string& image, const std::string& key) { return docker_get({"image", "inspect", image, "--format", "{{ index .Config.Labels \"com.mmltk." + key + "\" }}"}); }
 void verify_label(const std::string& image, const std::string& key, const std::string& expected) {
  const auto actual = image_label(image, key);
  require(actual == expected, "image '" + image + "' label '" + key + "' is '" + (actual.empty() ? "missing" : actual) + "', expected '" + expected + "'");
 }
 void ensure_builder(bool engine = false) {
  bool& ready = engine ? default_builder_ready : builder_ready;
  if (ready) return;
  require(docker_ok({"buildx", "version"}), "docker buildx is required");
  const auto name = engine ? "default" : builder;
  auto inspection = docker({"buildx", "inspect", name}, {.capture = true, .stderr_quiet = !engine, .merge = engine});
  if (inspection.status) {
   require(!engine, "failed to inspect Buildx builder 'default': " + trim(inspection.out));
   quiet_log("creating checkout-scoped Buildx builder '" + builder + "'");
   const auto creation = docker({"buildx", "create", "--name", builder, "--driver", "docker-container"}, {.capture = true, .merge = true});
   if (creation.status) {
    inspection = docker({"buildx", "inspect", builder}, {.capture = true, .stderr_quiet = true});
    require(!inspection.status, "failed to create Buildx builder '" + builder + "': " + trim(creation.out));
   }
  }
  if (!engine) {
   inspection = docker({"buildx", "inspect", builder}, {.capture = true});
   if (inspection.status) throw Exit{inspection.status};
  }
  std::string driver;
  for (const auto& line : support::lines(inspection.out))
   if (starts(line, "Driver:")) {
    auto fields = words(line);
    if (fields.size() > 1) driver = fields[1];
    break;
   }
  require(driver == (engine ? "docker" : "docker-container"),
   "Buildx builder '" + name + "' uses '" + (driver.empty() ? "unknown" : driver) + "', expected " + (engine ? "docker for Docker Engine image contexts" : "docker-container"));
  if (!engine) {
   auto result = docker({"buildx", "inspect", builder, "--bootstrap"}, {.capture = true});
   require(!result.status, "failed to bootstrap Buildx builder '" + builder + "'");
  }
  ready = true;
 }
 std::string image_alias(const std::string& component, const std::string& identity) {
  require(matches(identity, "sha256:[[:xdigit:]]{64}"), "image identity for '" + component + "' is not a Docker image ID: '" + identity + "'");
  auto alias = "mmltk-local-" + component + ":" + identity.substr(7);
  docker_must({"tag", identity, alias});
  verify_alias(alias, identity);
  return alias;
 }
 void verify_alias(const std::string& alias, const std::string& identity) {
  const auto result = docker({"image", "inspect", alias, "--format", "{{.Id}}"}, {.capture = true, .stderr_quiet = true});
  const auto actual = trim(result.out);
  require(!result.status && actual == identity, "local alias '" + alias + "' changed to '" + (actual.empty() ? "missing" : actual) + "', expected '" + identity + "'");
 }
 void cached_image(const std::string& selected_builder, const std::string& target, const std::string& tag, const std::string& dockerfile, const std::string& cache_name, const std::string& input,
  const std::string& context, Args extra = {}) {
  const auto event = cache_name == "runtime" ? "runtime_image" : "image:" + cache_name;
  const auto stamp = fingerprints + "/" + cache_name + ".sha256";
  std::string recorded_fingerprint;
  if (docker_ok({"image", "inspect", tag}))
   recorded_fingerprint = trim(docker({"image", "inspect", tag, "--format", "{{ index .Config.Labels \"com.mmltk.input-fingerprint\" }}"}, {.capture = true, .stderr_quiet = true}).out);
  if (docker_ok({"image", "inspect", tag}) && recorded_fingerprint == input) {
   owned_dirs({fingerprints});
   if (trim(read_optional(stamp)) != input) write_text(stamp, input + "\n");
   if (cache_name == "runtime") quiet_log("runtime image is current");
   trace_event(event, "cache_hit");
   return;
  }
  require(command_exists("docker"), "docker is not installed");
  Args cache_args;
  const auto image_cache = buildkit + "/" + cache_name;
  if (selected_builder == "default") {
   ensure_builder(true);
   owned_dirs({fingerprints});
  } else {
   ensure_builder();
   require(selected_builder == builder, "unsupported cached Buildx builder '" + selected_builder + "'");
   owned_dirs({fingerprints, image_cache});
   cache_args = {"--cache-to", "type=local,dest=" + image_cache + ",mode=max"};
   if (present(image_cache + "/index.json")) append(cache_args, {"--cache-from", "type=local,src=" + image_cache});
  }
  Args args{"buildx", "build", "--builder", selected_builder, "--progress=plain", "--load"};
  if (target != "-") {
   append(args, {"--target", target});
   quiet_log("building cached image '" + tag + "' from target '" + target + "'");
  } else
   quiet_log("building cached image '" + tag + "'");
  append(args, {"--tag", tag, "--file", dockerfile, "--build-arg", "MMLTK_IMAGE_FINGERPRINT=" + input});
  append(args, cache_args);
  append(args, extra);
  args.push_back(context);
  trace_event(event, "cache_miss");
  const int status = docker(std::move(args)).status;
  if (status) {
   log("failed to build cached image '" + tag + "' (exit " + std::to_string(status) + ")");
   throw Exit{status};
  }
  require(docker_ok({"image", "inspect", tag}), "image '" + tag + "' was not loaded");
  verify_label(tag, "input-fingerprint", input);
  write_text(stamp, input + "\n");
 }
 Args manifest_fields(const std::regex& pattern) {
  Args values;
  for (const auto& line : support::lines(read_text(repo + "/docker/nvidia-payload.json"))) {
   std::smatch match;
   if (std::regex_match(line, match, pattern)) values.push_back(match[1].str());
  }
  return values;
 }
 std::string manifest_value(const std::string& key) {
  const auto values = manifest_fields(std::regex(".*\"" + key + "\": \"([^\"]*)\".*"));
  require(values.size() == 1 && !values[0].empty(), "manifest key '" + key + "' is missing or ambiguous");
  return values[0];
 }
 void nvidia_inputs() {
  const auto canonical = manifest_value("donor");
  donor = env("MMLTK_NVIDIA_DONOR_IMAGE", canonical);
  require(donor == canonical && std::regex_search(donor, std::regex("@sha256:[[:xdigit:]]{64}$")), "MMLTK_NVIDIA_DONOR_IMAGE must match the canonical immutable donor in docker/nvidia-payload.json");
  require(manifest_value("platform") == "linux/amd64", "NVIDIA payload requires Linux/amd64");
  const auto orders = manifest_fields(std::regex(R"MMLTK(.*"library_order": \[(.*)\].*)MMLTK"));
  require(orders.size() == 1 && !orders[0].empty(), "missing or ambiguous NVIDIA library order");
  auto order = replace(replace(orders[0], "\"", ""), ",", " ");
  Args roots;
  for (const auto& key : words(order)) roots.push_back(manifest_value(key) + (key == "cuda_root" ? "/lib64" : "/lib"));
  nvidia_configuration = {
   "--build-arg", "MMLTK_CUDA_ROOT=" + manifest_value("cuda_root"), "--build-arg", "MMLTK_PYTHON_ROOT=" + manifest_value("python_root"), "--build-arg",
   "MMLTK_NVIDIA_LIBRARY_PATH=" + joined(roots, ":"), "--build-arg", "MMLTK_CUDA_VERSION=" + manifest_value("cuda"), "--build-arg", "MMLTK_TORCH_VERSION=" + manifest_value("torch"), "--build-arg",
   "MMLTK_TENSORRT_VERSION=" + manifest_value("tensorrt")
  };
 }
 void verify_payload() {
  verify_label(payload_image, "nvidia-donor", donor);
  verify_label(payload_image, "payload-mode", "extraction");
 }
 void verify_nvidia(const std::string& image, const std::string& payload_mode) {
  verify_label(image, "nvidia-donor", donor);
  verify_label(image, "payload-mode", payload_mode);
  for (const auto* component : {"cuda", "torch", "tensorrt"}) verify_label(image, std::string(component) + "-version", manifest_value(component));
 }
 void ensure_payload() {
  nvidia_inputs();
  const auto input = fingerprint({"nvidia-payload", donor}, {"docker/Dockerfile.nvidia", "docker/nvidia-payload.json", "docker/materialize_nvidia_payload.py", "docker/nvidia_payload.py"});
  cached_image(builder, "payload", payload_image, repo + "/docker/Dockerfile.nvidia", "nvidia-payload", input, repo,
   {"--platform", manifest_value("platform"), "--build-arg", "MMLTK_NVIDIA_DONOR_IMAGE=" + donor});
  verify_payload();
 }
 void ensure_gcc() {
  cached_image(builder, "gcc16", gcc_image, repo + "/docker/Dockerfile.gcc", "gcc16", fingerprint({"gcc16", "ubuntu:24.04"}, {"docker/Dockerfile.gcc"}), repo,
   {"--build-arg", "MMLTK_GCC_BUILD_IMAGE=ubuntu:24.04"});
 }
 void ensure_ubuntu() {
  if (!docker_ok({"image", "inspect", "ubuntu:24.04"})) docker_must({"pull", "--platform", "linux/amd64", "ubuntu:24.04"}, {.to_stderr = true});
  require(docker_get({"image", "inspect", "ubuntu:24.04", "--format", "{{.Os}}/{{.Architecture}}"}) == "linux/amd64", "owned Ubuntu base requires Linux/amd64");
  const auto id = image_id("ubuntu:24.04"), alias = image_alias("ubuntu-source", id);
  cached_image("default", "ubuntu", ubuntu_image, repo + "/docker/Dockerfile.ubuntu", "ubuntu", fingerprint({"ubuntu", id}, {"docker/Dockerfile.ubuntu"}), repo,
   {"--build-context", "mmltk_ubuntu=docker-image://" + alias, "--build-arg", "MMLTK_UBUNTU_IDENTITY=" + id});
  verify_alias(alias, id);
  verify_label(ubuntu_image, "ubuntu-identity", id);
  verify_label(ubuntu_image, "ubuntu-source-identity", id);
 }
 void verify_ort() {
  verify_label(ort_image, "ubuntu-identity", image_id(ubuntu_image));
  verify_label(ort_image, "gcc-identity", image_id(gcc_image));
  verify_label(ort_image, "nvidia-payload-identity", image_id(payload_image));
 }
 void ensure_ort() {
  const auto ubuntu = image_id(ubuntu_image), gcc = image_id(gcc_image), payload = image_id(payload_image);
  verify_payload();
  const auto ua = image_alias("ubuntu", ubuntu), ga = image_alias("gcc16", gcc), pa = image_alias("nvidia-payload", payload);
  Args args{
   "--build-context", "mmltk_ubuntu=docker-image://" + ua, "--build-context", "mmltk_gcc16=docker-image://" + ga, "--build-context", "mmltk_nvidia_payload=docker-image://" + pa, "--build-arg",
   "MMLTK_UBUNTU_IDENTITY=" + ubuntu, "--build-arg", "MMLTK_GCC_IDENTITY=" + gcc, "--build-arg", "MMLTK_NVIDIA_PAYLOAD_IDENTITY=" + payload, "--build-arg",
   "MMLTK_ONNXRUNTIME_BUILD_JOBS=" + std::to_string(std::min(jobs, 12L)), "--build-arg", "MMLTK_NVIDIA_NATIVE_ROOT=" + manifest_value("native_root")
  };
  append(args, nvidia_configuration);
  cached_image("default", "onnxruntime", ort_image, repo + "/docker/Dockerfile.onnxruntime", "onnxruntime",
   fingerprint({"onnxruntime", ubuntu, gcc, payload}, {"docker/Dockerfile.onnxruntime", "docker/patches/onnxruntime-1.27.1-cuda-graph-capture.patch", "docker/patches/onnxruntime-1.27.1-gcc16.patch"}),
   repo, args);
  verify_alias(ua, ubuntu);
  verify_alias(ga, gcc);
  verify_alias(pa, payload);
  verify_ort();
 }
 void ensure_toolchain_base() {
  const auto gcc = image_id(gcc_image), ubuntu = image_id(ubuntu_image), ga = image_alias("gcc16", gcc), ua = image_alias("ubuntu", ubuntu);
  cached_image("default", "toolchain", toolchain_image, repo + "/docker/Dockerfile.core", "core-toolchain",
   fingerprint({"toolchain-base", gcc, ubuntu},
    {"docker/Dockerfile.core", "docker/patches/libdeflate-gcc16.patch", "docker/install_trunk.py", "docker/record_compiler_evidence.py", "src/frontend/iced/wasm-opt-wrapper"}),
   repo,
   {"--build-context", "mmltk_gcc16=docker-image://" + ga, "--build-context", "mmltk_ubuntu=docker-image://" + ua, "--build-arg", "MMLTK_GCC_IDENTITY=" + gcc, "--build-arg",
    "MMLTK_UBUNTU_IDENTITY=" + ubuntu});
  verify_alias(ga, gcc);
  verify_alias(ua, ubuntu);
  verify_label(toolchain_image, "gcc-identity", gcc);
  verify_label(toolchain_image, "ubuntu-identity", ubuntu);
 }
 void ensure_dependencies() {
  const auto id = image_id(toolchain_image), alias = image_alias("toolchain-base", id);
  cached_image("default", "dependencies", dependencies_image, repo + "/docker/Dockerfile.dependencies", "dependencies", fingerprint({"dependencies", id}, {"docker/Dockerfile.dependencies"}), repo,
   {"--build-context", "mmltk_toolchain_base=docker-image://" + alias, "--build-arg", "MMLTK_TOOLCHAIN_BASE_IDENTITY=" + id});
  verify_alias(alias, id);
  verify_label(dependencies_image, "toolchain-base-identity", id);
 }
 void prepare_producers() {
  nvidia_inputs();
  ensure_builder();
  ensure_builder(true);
  ensure_ubuntu();
  task([&] { traced("nvidia_payload", [&] { ensure_payload(); }); });
  task([&] { traced("gcc16", [&] { ensure_gcc(); }); });
  wait_tasks();
  task([&] {
   traced("host_dependencies", [&] {
    ensure_toolchain_base();
    ensure_dependencies();
   });
  });
  task([&] { traced("onnxruntime", [&] { ensure_ort(); }); });
  wait_tasks();
 }
 void ensure_build_image() {
  nvidia_inputs();
  if (!producers_prepared) {
   prepare_producers();
   producers_prepared = true;
  }
  const auto base = image_id(toolchain_image), payload = image_id(payload_image), dependency = image_id(dependencies_image), ort = image_id(ort_image);
  verify_label(dependencies_image, "toolchain-base-identity", base);
  verify_payload();
  verify_ort();
  const auto da = image_alias("dependencies", dependency), pa = image_alias("nvidia-payload", payload), oa = image_alias("onnxruntime", ort);
  const auto compilation = "sha256:" + fingerprint({"compilation", dependency, payload, ort}, {"docker/Dockerfile.toolchain", "docker/patches/gcc16-nvcc-string-substr.patch"});
  Args args{
   "--build-context", "mmltk_dependencies=docker-image://" + da, "--build-context", "mmltk_nvidia_payload=docker-image://" + pa, "--build-context", "mmltk_onnxruntime=docker-image://" + oa,
   "--build-arg", "MMLTK_COMPILATION_IDENTITY=" + compilation, "--build-arg", "MMLTK_TOOLCHAIN_BASE_IDENTITY=" + base, "--build-arg", "MMLTK_DEPENDENCIES_IDENTITY=" + dependency, "--build-arg",
   "MMLTK_ONNXRUNTIME_IDENTITY=" + ort, "--build-arg", "MMLTK_NVIDIA_PAYLOAD_IDENTITY=" + payload, "--build-arg", "MMLTK_NVIDIA_DONOR_IMAGE=" + donor
  };
  append(args, nvidia_configuration);
  append(args, {"--build-context", "mmltk_tools=" + repo + "/build/tools"});
  cached_image("default", "-", build_image, repo + "/docker/Dockerfile.toolchain", "toolchain",
   fingerprint({"toolchain", base, dependency, payload, ort, trim(read_optional(repo + "/build/tools/.inputs.sha256"))},
    {"docker/Dockerfile.toolchain", "docker/patches/gcc16-nvcc-string-substr.patch", "docker/verify_nvidia_payload.py", "docker/nvidia_payload.py", "tools/toolchain/check_toolchain_invariants.cpp"}),
   repo, args);
  verify_alias(da, dependency);
  verify_alias(pa, payload);
  verify_alias(oa, ort);
  for (const auto& [key, id] : std::map<std::string, std::string>{
        {"toolchain-base-identity", base}, {"compilation-identity", compilation}, {"dependencies-identity", dependency}, {"onnxruntime-identity", ort}, {"nvidia-payload-identity", payload}
       })
   verify_label(build_image, key, id);
  verify_nvidia(build_image, "development");
 }
 void ensure_runtime_base() {
  nvidia_inputs();
  if (!producers_prepared) {
   ensure_ubuntu();
   ensure_gcc();
   ensure_payload();
   ensure_ort();
  }
  const auto payload = image_id(payload_image), ubuntu = image_id(ubuntu_image), ort = image_id(ort_image);
  verify_payload();
  verify_ort();
  const auto pa = image_alias("nvidia-payload", payload), ua = image_alias("ubuntu", ubuntu), oa = image_alias("onnxruntime", ort);
  Args args{
   "--build-context", "mmltk_nvidia_payload=docker-image://" + pa, "--build-context", "mmltk_ubuntu=docker-image://" + ua, "--build-context", "mmltk_onnxruntime=docker-image://" + oa, "--build-arg",
   "MMLTK_NVIDIA_DONOR_IMAGE=" + donor, "--build-arg", "MMLTK_NVIDIA_PAYLOAD_IDENTITY=" + payload, "--build-arg", "MMLTK_UBUNTU_IDENTITY=" + ubuntu, "--build-arg", "MMLTK_ONNXRUNTIME_IDENTITY=" + ort
  };
  append(args, nvidia_configuration);
  cached_image("default", "runtime-base", runtime_base, repo + "/docker/Dockerfile.runtime-base", "runtime-base",
   fingerprint({"runtime-base", payload, ubuntu, ort}, {"docker/Dockerfile.runtime-base", "docker/verify_nvidia_payload.py", "docker/nvidia_payload.py"}), repo, args);
  verify_alias(pa, payload);
  verify_alias(ua, ubuntu);
  verify_alias(oa, ort);
  verify_label(runtime_base, "nvidia-payload-identity", payload);
  verify_label(runtime_base, "onnxruntime-identity", ort);
  verify_label(runtime_base, "ubuntu-identity", ubuntu);
  verify_nvidia(runtime_base, "runtime");
 }
 void ensure_analysis() {
  ensure_build_image();
  const auto build_id = image_id(build_image);
  const auto current = docker({"image", "inspect", analysis_image, "--format", "{{.Id}}"}, {.capture = true, .stderr_quiet = true});
  if (trim(current.out) != build_id || current.status) {
   quiet_log("tagging source-free toolchain as analysis image '" + analysis_image + "'");
   docker_must({"tag", build_image, analysis_image});
  }
 }
 std::string host_path(const std::string& value) {
  if (starts(value, "/host/")) return value;
  return "/host" + (starts(value, "/") ? value : normalized(host_workdir + "/" + value));
 }
 std::string benchmark_cache(std::string value) {
  if (starts(value, "/host/"))
   value.erase(0, 5);
  else if (!starts(value, "/"))
   value = host_workdir + "/" + value;
  value = normalized(value);
  require(within(value, repo), "--cache-dir must stay within " + repo);
  return "/host" + value;
 }
 static bool path_option(const std::string& name) {
  static const std::set<std::string> options{
   "--source-dir", "--output-dir", "--cache-dir", "--compiled", "--weights", "--class-layout", "--resume", "--onnx", "--tensorrt", "--output", "--input", "--save-engine", "--report-json", "--source",
   "--train-compiled", "--val-compiled", "--test-compiled", "--dist-store-file", "--file", "--start-at", "--log-file", "--log-dir", "source_dir", "output_dir", "compiled"
  };
  return options.contains(name);
 }
 void rewrite_args() {
  rewritten.clear();
  bool expect_path = false, expect_cache = false, expect_files = false;
  for (const auto& arg : passthrough) {
   if (expect_cache) {
    rewritten.push_back(benchmark_cache(arg));
    expect_cache = false;
    continue;
   }
   if (expect_path) {
    rewritten.push_back(host_path(arg));
    expect_path = false;
    continue;
   }
   if (expect_files) {
    if (!starts(arg, "--")) {
     rewritten.push_back(host_path(arg));
     continue;
    }
    expect_files = false;
   }
   if (starts(arg, "--cache-dir=")) {
    rewritten.push_back("--cache-dir=" + benchmark_cache(arg.substr(12)));
    continue;
   }
   if (arg == "--cache-dir") {
    rewritten.push_back(arg);
    expect_cache = true;
    continue;
   }
   if ((tidy || tidy_tools) && arg == "--file") {
    rewritten.push_back(arg);
    expect_files = true;
    continue;
   }
   if (starts(arg, "--") && arg.find('=') != std::string::npos) {
    const auto at = arg.find('=');
    const auto name = arg.substr(0, at);
    rewritten.push_back(path_option(name) ? name + "=" + host_path(arg.substr(at + 1)) : arg);
    continue;
   }
   if (path_option(arg)) {
    rewritten.push_back(arg);
    expect_path = true;
    continue;
   }
   rewritten.push_back(starts(arg, "/") ? host_path(arg) : arg);
  }
 }
 static void execution_policy(Args& args) {
  append(args, {"--cap-add", "SYS_NICE", "--cap-add", "IPC_LOCK", "--ulimit", "memlock=-1:-1", "--ulimit", "nice=40:40"});
  struct stat info{};
  if (!stat("/dev/gdrdrv", &info) && S_ISCHR(info.st_mode)) append(args, {"--device", "/dev/gdrdrv"});
 }
 void native_environment(Args& args) {
  for (const auto* name : {"GDRCOPY_USE_DMABUF_MMAP", "GDRCOPY_ENABLE_LOGGING", "GDRCOPY_LOG_LEVEL", "MMLTK_ANNOTATION_DIAGNOSTICS", "MMLTK_UPSCALE_ONNX_REFERENCE"})
   if (auto value = env(name); !value.empty()) append(args, {"--env", std::string(name) + "=" + value});
  for (const auto* name : {"MMLTK_GDR_TRACE_FILE", "MMLTK_NUMA_TRANSFER_TRACE_FILE"})
   if (auto value = env(name); !value.empty()) append(args, {"--env", std::string(name) + "=" + host_path(value)});
 }
 void logging_environment(Args& args, const std::string& logging_mode, const Args& program_args) {
  std::map<std::string, std::string> values, entries;
  const Args names{"MMLTK_LOG_LEVEL", "MMLTK_LOG_FILE", "MMLTK_LOG_DIR"};
  for (const auto& name : names)
   if (auto value = env(name); !value.empty()) {
    if (name != "MMLTK_LOG_LEVEL") value = host_path(value);
    values[name] = value;
    entries[name] = name + "=" + value;
   }
  if (logging_mode != "exec")
   for (const auto& entry : test_environment) {
    const auto name = entry.substr(0, entry.find('='));
    if (std::find(names.begin(), names.end(), name) != names.end()) {
     values[name] = entry.find('=') == std::string::npos ? env(name) : entry.substr(entry.find('=') + 1);
     entries[name] = entry;
    }
   }
  std::string level = values["MMLTK_LOG_LEVEL"];
  bool valid = matches(lower(level), "(trace|debug|info|warn|warning|error|err|critical|off)?"), destination = !values["MMLTK_LOG_FILE"].empty() || !values["MMLTK_LOG_DIR"].empty();
  for (std::size_t i = 0; valid && i < program_args.size(); ++i) {
   std::string option = program_args[i], value;
   const auto at = option.find('=');
   if (at != std::string::npos) {
    value = option.substr(at + 1);
    option.resize(at);
   } else if (option == "--log-level" || option == "--log-file" || option == "--log-dir") {
    if (++i == program_args.size()) continue;
    value = program_args[i];
   } else
    continue;
   if (option == "--log-level") {
    level = value.empty() ? values["MMLTK_LOG_LEVEL"] : value;
    valid = matches(lower(level), "(trace|debug|info|warn|warning|error|err|critical|off)?");
   } else if (option == "--log-file" || option == "--log-dir")
    destination = true;
  }
  const bool enabled = lower(level) != "off" && (level.empty() ? logging_mode == "wayland" : valid);
  if (enabled && !destination) {
   owned_dirs({repo + "/.mmltk-data/logs"});
   entries["MMLTK_LOG_DIR"] = "MMLTK_LOG_DIR=" + (logging_mode == "wayland" ? "/workspace/.mmltk-data/logs" : host_path(repo + "/.mmltk-data/logs"));
  }
  for (const auto& name : names)
   if (entries.contains(name)) append(args, {"--env", entries[name]});
 }
 void common_groups() {
  groups.clear();
  std::string name = env("MMLTK_USER");
  if (name.empty() && uid != getuid()) {
   if (auto* entry = getpwuid(uid)) name = entry->pw_name;
  }
  Args args{"id", "-G"};
  if (!name.empty()) args.push_back(name);
  auto ids = words(get(args));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (const auto& id : ids) append(groups, {"--group-add", id});
  const auto video = capture({"getent", "group", "video"}, true);
  const auto fields = support::split(trim(video.out), ':');
  if (fields.size() >= 3 && !fields[2].empty()) append(groups, {"--group-add", fields[2]});
 }
 void infer_value(const std::string& name, const std::string& value, const std::string& suffix = {}) {
  if (!value.empty() && env(name).empty()) {
   setenv(name.c_str(), value.c_str(), 1);
   quiet_log("assuming " + name + "=" + value + suffix);
  }
 }
 void infer_gui() {
  Args proc_dirs;
  for (const auto& entry : fs::directory_iterator("/proc"))
   if (matches(entry.path().filename().string(), "[0-9]+")) proc_dirs.push_back(entry.path().string());
  std::sort(proc_dirs.begin(), proc_dirs.end());
  for (const auto& directory : proc_dirs) {
   struct stat info{};
   if (stat(directory.c_str(), &info) || info.st_uid != uid) continue;
   const auto text = read_optional(directory + "/environ");
   const auto entries = split_zero(text);
   for (const auto* name : {"XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"})
    if (env(name).empty())
     for (const auto& entry : entries)
      if (starts(entry, std::string(name) + "=")) {
       infer_value(name, entry.substr(std::strlen(name) + 1));
       break;
      }
   if (!env("XDG_RUNTIME_DIR").empty() && !env("WAYLAND_DISPLAY").empty() && !env("DBUS_SESSION_BUS_ADDRESS").empty()) break;
  }
  const auto default_runtime = "/run/user/" + std::to_string(uid);
  if (env("XDG_RUNTIME_DIR").empty() && fs::is_directory(default_runtime)) infer_value("XDG_RUNTIME_DIR", default_runtime);
  if (const auto runtime = env("XDG_RUNTIME_DIR"); !runtime.empty() && present(runtime + "/bus")) infer_value("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + runtime + "/bus");
  if (env("XDG_RUNTIME_DIR").empty() && starts(env("WAYLAND_DISPLAY"), "/")) infer_value("XDG_RUNTIME_DIR", parent(env("WAYLAND_DISPLAY")));
  if (env("WAYLAND_DISPLAY").empty()) {
   const auto runtime = env("XDG_RUNTIME_DIR");
   std::string display;
   if (!runtime.empty()) {
    if (present(runtime + "/wayland-0"))
     display = "wayland-0";
    else if (fs::is_directory(runtime)) {
     Args candidates;
     for (const auto& item : fs::directory_iterator(runtime)) {
      auto name = item.path().filename().string();
      if (starts(name, "wayland-") && !name.ends_with(".lock") && present(item.path().string())) candidates.push_back(name);
     }
     std::sort(candidates.begin(), candidates.end());
     if (!candidates.empty()) display = candidates[0];
    }
   }
   if (display.empty())
    infer_value("WAYLAND_DISPLAY", "wayland-0");
   else
    infer_value("WAYLAND_DISPLAY", display, " from " + runtime + "/" + display);
  }
 }
 void resolve_wayland() {
  infer_gui();
  const auto display = env("WAYLAND_DISPLAY"), runtime = env("XDG_RUNTIME_DIR");
  require(!display.empty(), "--gui requires WAYLAND_DISPLAY to be set on the host");
  require(!runtime.empty(), "--gui requires XDG_RUNTIME_DIR to be set on the host");
  wayland_host = starts(display, "/") ? display : runtime + "/" + display;
  require(present(wayland_host), "--gui requires the Wayland socket '" + runtime + "/" + display + "' to exist on the host");
  gui_platform = "wayland";
  wayland_container = gui_runtime + "/" + filename(wayland_host);
  dbus_host.clear();
  dbus_container.clear();
  const auto address = env("DBUS_SESSION_BUS_ADDRESS");
  if (starts(address, "unix:path=")) {
   const auto path = address.substr(10, address.find(',', 10) == std::string::npos ? std::string::npos : address.find(',', 10) - 10);
   if (present(path)) {
    dbus_host = path;
    dbus_container = gui_runtime + "/" + filename(path);
   } else
    log("warning: DBUS session bus socket '" + path + "' is unavailable; skipping DBUS session bus forwarding");
  }
 }
 void resolve_gui() {
  if (gui && gui_platform.empty()) resolve_wayland();
 }
 std::string tmpfs() const { return gui_runtime + ":rw,mode=700,uid=" + std::to_string(uid) + ",gid=" + std::to_string(gid); }
 void bind(const std::string& source, std::string target = {}, const std::string& access = "rw") {
  if (!present(source)) return;
  for (const auto& spec : mounts)
   if (within(source, spec.substr(0, spec.find(':')))) return;
  mounts.push_back(source + ":" + (target.empty() ? source : target) + ":" + access);
 }
 void prepare_mounts(bool workspace = false) {
  mounts.clear();
  bind("/dev");
  if (!gui && !workspace) return;
  if (workspace && headless) {
   gui_platform = "wayland";
   wayland_container = gui_runtime + "/wayland-mmltk";
   return;
  }
  if (workspace)
   resolve_wayland();
  else
   resolve_gui();
  bind(wayland_host, wayland_container);
  if (!dbus_host.empty()) bind(dbus_host, dbus_container);
  if (fs::is_directory("/run/dbus")) bind("/run/dbus");
 }
 void append_gui_environment(Args& args) {
  append(args, {"--env", "XDG_SESSION_TYPE=" + gui_platform, "--env", "XDG_RUNTIME_DIR=" + gui_runtime, "--env", "GDK_BACKEND=wayland", "--env",
                "__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json", "--env", "GBM_BACKEND=nvidia-drm", "--env", "WAYLAND_DISPLAY=" + filename(wayland_container)});
  if (!dbus_container.empty() && !env("DBUS_SESSION_BUS_ADDRESS").empty())
   append(args, {"--env", "DBUS_SESSION_BUS_ADDRESS=" + replace(env("DBUS_SESSION_BUS_ADDRESS"), "unix:path=" + dbus_host, "unix:path=" + dbus_container)});
  for (const auto* name : {"__NV_PRIME_RENDER_OFFLOAD", "MMLTK_GUI_PIXEL_TRACE"})
   if (!env(name).empty()) append(args, {"--env", std::string(name) + "=" + env(name)});
  for (const auto* name : {"MMLTK_GUI_TRACE_FILE", "MMLTK_FIREFOX_LOG_FILE"})
   if (!env(name).empty()) append(args, {"--env", std::string(name) + "=" + host_path(env(name))});
 }
 Args runtime_args() {
  Args args{
   "run", "-d", "--init", "--privileged", "--name", container_name, "--entrypoint", "/bin/sh", "--gpus", "all", "--network", "host", "--ipc", "host", "--user", user(), "--env", "HOME=/tmp/mmltk-home",
   "--env", "NVIDIA_DRIVER_CAPABILITIES=all", "--label", "com.mmltk.runtime=" + runtime_label, "--volume", "/:/host:rw", "--workdir", "/host" + repo
  };
  diagnostic_tools(args);
  execution_policy(args);
  labels(args, mode);
  if (gui) append(args, {"--tmpfs", tmpfs()});
  common_groups();
  append(args, groups);
  for (const auto& mount : mounts) append(args, {"--volume", mount});
  append(args, {image_name, "-lc", "mkdir -p \"${HOME}\" && exec sleep infinity"});
  return args;
 }
 void ensure_container() {
  ensure_docker();
  ensure_image();
  prepare_mounts();
  Args specs{"image=" + image_id(image_name), "privileged=1", "user=" + user(), "home=/tmp/mmltk-home"};
  if (gui)
   append(specs, {"gui=1", "gui_platform=" + gui_platform, "tmpfs=" + tmpfs()});
  else
   specs.push_back("gui=0");
  for (const auto& mount : mounts) specs.push_back("mount=" + mount);
  runtime_label = joined(specs, "|");
  auto exists = [&] { return docker_ok({"container", "inspect", container_name}); };
  auto running = [&] { return trim(docker({"inspect", "--format", "{{.State.Running}}", container_name}, {.capture = true, .stderr_quiet = true}).out) == "true"; };
  if (exists() && trim(docker({"inspect", "--format", "{{ index .Config.Labels \"com.mmltk.runtime\" }}", container_name}, {.capture = true, .stderr_quiet = true}).out) != runtime_label) {
   quiet_log("recreating " + container_name + " for updated runtime configuration");
   docker({"rm", "-f", container_name}, {.quiet = true});
  }
  if (!exists()) {
   docker_must(runtime_args(), {.capture = true});
   return;
  }
  if (!running()) docker({"start", container_name}, {.capture = true});
  if (!running()) {
   quiet_log("recreating " + container_name + " because it did not stay running");
   docker({"rm", "-f", container_name}, {.quiet = true});
   docker_must(runtime_args(), {.capture = true});
  }
  if (gui)
   docker_must({"exec", "-u", "0", container_name, "/bin/sh", "-lc",
                "mkdir -p " + shell_quote(gui_runtime) + " && chown " + shell_quote(user()) + " " + shell_quote(gui_runtime) + " && chmod 700 " + shell_quote(gui_runtime)},
    {.capture = true});
 }
 void prepare_invocation() {
  cleanup_armed = true;
  ensure_container();
  Args environment;
  native_environment(environment);
  append(environment, {"-e", "MMLTK_RFDETR_TEST_CACHE_DIR=/host" + cache + "/tests/rfdetr", "-e", "MMLTK_BENCHMARK_DATASET_CACHE_ROOT=/host" + cache + "/benchmark-dataset/v1"});
  logging_environment(environment, "exec", rewritten);
  if (gui) {
   resolve_gui();
   append_gui_environment(environment);
   require(!docker({"exec", container_name, "/bin/sh", "-lc", std::string(commands::contract), "mmltk-contract-check", "/opt/mmltk/share/mmltk/browser-app"}).status,
    "runtime image is missing the packaged browser host, browser-bundle verifier, Firefox runtime, GUI assets, or native protocol marker; rebuild with ./mmltk --build");
   require(
    !docker({"exec", container_name, "/opt/mmltk/bin/mmltk-browser-bundle-contract", "verify", "/opt/mmltk/share/mmltk/browser-app", "/opt/mmltk/share/mmltk/browser-app/browser_protocol.marker"})
     .status,
    "the packaged GUI browser bundle contract is invalid or incompatible with the native host; rebuild with ./mmltk --build");
  }
  execution_args = {"exec", "-i"};
  const bool tty = isatty(0) && isatty(1);
  if (tty) execution_args.push_back("-t");
  append(execution_args, environment);
  append(execution_args, {"-w", "/host" + host_workdir, container_name});
  container_command = tty ? Args{} : Args{"stdbuf", "-oL", "-eL"};
  container_command.push_back(gui ? "/opt/mmltk/bin/mmltk-browser-host" : "/opt/mmltk/bin/mmltk");
  append(container_command, rewritten);
  if (gui) quiet_log("using packaged browser host and GUI bundle");
 }
 Result toolchain_command(const std::string& command, const std::string& owned_name = {}, const std::string& capabilities = {}, Run options = {}) {
  compiler_cache_dirs();
  const auto identity = image_label(build_image, "compilation-identity");
  require(matches(identity, "sha256:[[:xdigit:]]{64}"), "build image lacks its validated compilation identity");
  Args args{"run", "--rm", "--init", "--pull", "never", "--security-opt", "seccomp=unconfined", "--cpuset-cpus", cpu_list, "--user", user()};
  const Args variables{
   "HOME=" + container_cache + "/container-home", "CARGO_HOME=" + container_cache + "/cargo/home", "CARGO_TARGET_DIR=" + container_cache + "/cargo/target/browser-app",
   "CARGO_BUILD_JOBS=" + std::to_string(jobs), "CMAKE_BUILD_PARALLEL_LEVEL=" + std::to_string(jobs), "CCACHE_BASEDIR=/workspace", "CCACHE_DIR=" + container_cache + "/ccache",
   "CCACHE_TEMPDIR=" + container_cache + "/ccache/tmp", "CCACHE_NOHASHDIR=1", "CCACHE_MAXSIZE=50G", "CCACHE_SLOPPINESS=pch_defines,time_macros", "MMLTK_CONTAINER_CACHE_ROOT=" + container_cache,
   "MMLTK_CONTAINER_RELEASE_STAGE_ROOT=" + container_stage, "MMLTK_FIREFOX_CACHE_ROOT=" + container_cache + "/firefox", "MMLTK_JOBS=" + std::to_string(jobs), "MMLTK_TOOLCHAIN_ID=" + identity,
   "MOZBUILD_STATE_PATH=" + container_cache + "/firefox/mozbuild", "SCCACHE_BASEDIRS=" + sccache_basedirs, "SCCACHE_DIR=" + container_cache + "/firefox/sccache", "SCCACHE_CACHE_SIZE=20G"
  };
  for (const auto& value : variables) append(args, {"--env", value});
  append(args, {"--volume", repo + ":/workspace:rw", "--workdir", "/workspace", "--entrypoint", "/bin/bash"});
  labels(args, mode);
  if (!owned_name.empty()) append(args, {"--name", owned_name});
  for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
        {"MMLTK_PACKAGE_FINGERPRINT", env("MMLTK_PACKAGE_FINGERPRINT")}, {"MMLTK_CCACHE_STATS_FILE", container_ccache_stats}, {"MMLTK_FIREFOX_SCCACHE_STATS_FILE", container_sccache_stats},
        {"MMLTK_BUILD_TRACE_FILE", container_trace}, {"MMLTK_FIREFOX_STAGE_LOG_FILE", container_firefox_trace}
       })
   if (!value.empty()) append(args, {"--env", name + "=" + value});
  if (mode == "export-shiftlut") {
   append(args, {"--volume", shiftlut_source + ":/shiftlut-source:ro", "--env", "MMLTK_SHIFTLUT_MODE=" + shiftlut_operation});
   if (shiftlut_operation == "--verify-only") append(args, {"--gpus", "all", "--env", "NVIDIA_DRIVER_CAPABILITIES=compute,utility"});
  }
  if (!capabilities.empty()) append(args, {"--gpus", "all", "--env", "NVIDIA_DRIVER_CAPABILITIES=" + capabilities});
  append(args, {build_image, "-lc", command});
  return docker(std::move(args), std::move(options));
 }
 int owned_toolchain(const std::string& name, std::string_view command, const std::string& capabilities = {}, Run options = {}) {
  return owned_container(name, [&] { return toolchain_command(std::string(command), name, capabilities, std::move(options)); }).status;
 }
 void must_toolchain(const std::string& name, std::string_view command, const std::string& capabilities = {}, Run options = {}) {
  const auto status = owned_toolchain(name, command, capabilities, std::move(options));
  if (status) throw Exit{status};
 }
 std::string file_fingerprint(const std::string& root) {
  std::string data;
  auto emit = [&](std::string_view value) {
   data += value;
   data.push_back('\0');
  };
  auto member = [&](const std::string& path, const std::string& relative) {
   struct stat info{};
   require(lstat(path.c_str(), &info) == 0, "cannot stat '" + path + "'");
   std::ostringstream mode_string;
   mode_string << std::oct << (info.st_mode & 07777);
   emit(mode_string.str());
   if (!relative.empty()) emit(relative);
   if (S_ISLNK(info.st_mode)) {
    emit("link");
    emit(fs::read_symlink(path).string());
   } else {
    const auto sum = get({"sha256sum", "--", path});
    data += sum.substr(0, sum.find(' ')) + "\n";
   }
  };
  if (fs::is_directory(root)) {
   emit("directory");
   Args paths;
   for (const auto& item : fs::recursive_directory_iterator(root)) {
    std::error_code ec;
    const auto status = item.symlink_status(ec);
    if (fs::is_regular_file(status) || fs::is_symlink(status)) paths.push_back("./" + item.path().lexically_relative(root).string());
   }
   for (const auto& path : sorted_paths(std::move(paths))) member(root + "/" + path, path);
  } else if (fs::is_regular_file(root) || symlink(root)) {
   emit("file");
   member(root, {});
  } else
   data = "missing\n";
  return sha(std::move(data));
 }
 std::string package_hash(const std::string& path) { return get(tool_arguments(Command::runtime_package, {"path-fingerprint", path}, self)); }
 bool package_complete(const std::string& path) { return package::complete(path); }
 void firefox_build_input() {
  const auto identity = image_id(toolchain_image);
  require(image_label(build_image, "toolchain-base-identity") == identity, "final toolchain image does not identify its completed dependency image");
  std::string data;
  auto emit = [&](const std::string& value) {
   data += value;
   data.push_back('\0');
  };
  emit("mmltk-firefox-build-input-v3");
  emit(identity);
  for (const auto& [name, path] : std::vector<std::pair<std::string, std::string>>{
        {"desktop-build-definition", "src/entrypoints/desktop/firefox_runtime_build.cmake"}, {"firefox-build-command", "src/entrypoints/desktop/build_firefox_runtime.sh"},
        {"firefox-runtime-manifest", "src/entrypoints/desktop/firefox_runtime_files.txt"}
       }) {
   emit(name);
   emit(file_fingerprint(repo + "/" + path));
  }
  for (const Args& command :
   std::vector<Args>{{"git", "-C", repo, "rev-parse", "HEAD:third_party/firefox"}, {"git", "-C", repo, "diff", "--no-ext-diff", "--binary", "HEAD", "--", "third_party/firefox"}}) {
   const auto result = capture(command);
   if (result.status) throw Exit{result.status};
   data += result.out;
  }
  const auto untracked = capture({"git", "-C", repo, "ls-files", "--others", "--exclude-standard", "-z", "--", "third_party/firefox"});
  if (untracked.status) throw Exit{untracked.status};
  for (const auto& path : split_zero(untracked.out))
   if (!path.empty()) {
    emit("untracked");
    emit(path);
    emit(file_fingerprint(repo + "/" + path));
   }
  const auto hash = sha(std::move(data));
  const auto next = firefox_input + ".next." + std::to_string(wrapper_pid);
  write_text(next, hash + "\n");
  if (read_optional(firefox_input) == hash + "\n")
   fs::remove(next);
  else {
   fs::rename(next, firefox_input);
   chown(firefox_input.c_str(), uid, gid);
   quiet_log("Firefox build inputs changed");
  }
 }
 std::string package_fingerprint() {
  const auto root = cmake + "/release";
  std::string data = "mmltk-release-package-v8\n" + image_id(build_image) + "\n" + sha_files({repo + "/tools/toolchain/runtime_package.cpp"});
  for (const auto& row : package::inventory()) {
   const std::string path(row.path), kind(row.source_kind), source(row.source_path);
   std::string source_root;
   if (kind == "release")
    source_root = root;
   else if (kind == "repository")
    source_root = repo + "/" + source;
   else if (kind == "firefox")
    source_root = firefox + "/obj-minimal-opt/dist/firefox";
   else if (kind == "browser")
    source_root = root + "/frontend/iced/dist";
   else if (kind == "image")
    continue;
   else
    die("unknown runtime package source '" + kind + "'");
   if (kind == "release" || kind == "repository") {
    for (auto entry : row.entries) {
     if (entry.starts_with('?')) entry.remove_prefix(1);
     data += path + "/" + std::string(entry) + " " + package_hash(source_root + "/" + std::string(entry)) + "\n";
    }
   } else {
    data += path + " " + package_hash(source_root) + "\n";
    if (kind == "browser") data += source + " " + package_hash(root + "/" + source) + "\n";
   }
  }
  Args installation_files;
  if (present(root))
   for (const auto& item : fs::recursive_directory_iterator(root))
    if (fs::is_regular_file(item.symlink_status()) && item.path().filename() == "cmake_install.cmake") installation_files.push_back(item.path().string());
  for (const auto& path : sorted_paths(std::move(installation_files))) data += path.substr(root.size() + 1) + " " + file_fingerprint(path) + "\n";
  return sha(std::move(data));
 }
 void runtime_image(const std::string& stage) {
  const auto root = cmake + "/release";
  require(read_optional(root + "/build.ninja").find("build mmltk_frontend_iced_bundle: phony") != std::string::npos &&
           read_optional(root + "/CMakeCache.txt").find("BUILD_MMLTK_BROWSER_HOST:BOOL=ON") != std::string::npos &&
           read_optional(root + "/CMakeCache.txt").find("BUILD_MMLTK_FIREFOX_RUNTIME:BOOL=ON") != std::string::npos,
   "release graph must enable the browser host, browser app, and owned Firefox runtime");
  require(package_complete(stage), "staged release is incomplete or contains test executables");
  const auto id = image_id(runtime_base);
  nvidia_inputs();
  verify_nvidia(runtime_base, "runtime");
  const auto package_result = capture(tool_arguments(Command::runtime_package, {"fingerprint", stage}, self));
  if (package_result.status) throw Exit{package_result.status};
  const auto input = sha("mmltk-runtime-image-v6\n" + id + "\n" + sha_files({repo + "/docker/Dockerfile.runtime", repo + "/tools/toolchain/runtime_package.cpp"}) + package_result.out +
                         trim(read_optional(repo + "/build/tools/.inputs.sha256"))),
             alias = image_alias("runtime-base", id);
  cached_image("default", "-", image_name, repo + "/docker/Dockerfile.runtime", "runtime", input, stage,
   {"--build-context", "mmltk_tools=" + repo + "/build/tools", "--build-arg", "MMLTK_RUNTIME_BASE_IMAGE=" + alias, "--build-arg", "MMLTK_RELEASE_FINGERPRINT=" + input, "--build-arg",
    "MMLTK_RUNTIME_BASE_IDENTITY=" + id});
  verify_alias(alias, id);
  require(image_label(image_name, "release-fingerprint") == input, "runtime image release fingerprint label does not match staged outputs");
  require(image_label(image_name, "runtime-base-identity") == id, "runtime image base identity label does not match " + runtime_base);
  verify_nvidia(image_name, "runtime");
 }
 void stage_release() {
  const auto input = package_fingerprint();
  if (package_complete(release_stage) && trim(read_optional(release_stage + "/.mmltk-package-fingerprint")) == input) {
   quiet_log("staged release is current");
   trace_event("package", "cache_hit");
   runtime_image(release_stage);
   return;
  }
  trace_event("package", "cache_miss");
  const auto next = release_stage + ".next";
  setenv("MMLTK_PACKAGE_FINGERPRINT", input.c_str(), 1);
  quiet_log("installing release into " + next);
  must_toolchain(package_container, commands::package);
  unsetenv("MMLTK_PACKAGE_FINGERPRINT");
  require(package_complete(next), "staged release is incomplete");
  require(trim(read_optional(next + "/.mmltk-package-fingerprint")) == input, "staged release fingerprint is incomplete");
  runtime_image(next);
  const auto previous = release_stage + ".previous";
  fs::remove_all(previous);
  if (present(release_stage)) fs::rename(release_stage, previous);
  fs::rename(next, release_stage);
  fs::remove_all(previous);
 }
 void prepare_tools(bool force) {
  const auto name = base_name + "-tools-bootstrap-" + std::to_string(wrapper_pid);
  const auto result = owned_container(name, [&] {
   return run({"bash", repo + "/tools/bootstrap/bootstrap_mmltk.sh", force ? "--build" : "--ensure", repo},
    {.environment = {{"MMLTK_BOOTSTRAP_CAPTURED", "1"}, {"MMLTK_BOOTSTRAP_CONTAINER", name}, {"MMLTK_BOOTSTRAP_OWNER_PID", std::to_string(wrapper_pid)}, {"MMLTK_BOOTSTRAP_USER", user()}}});
  });
  if (result.status) throw Exit{result.status};
 }
 void run_build() {
  acquire_mutation();
  prepare_tools(false);
  initialize_trace();
  trace_event("build", "start");
  if (trace_enabled) {
   support::json record{
    {"timestamp_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()}, {"pid", getpid()}, {"phase", "jobs"}, {"event", "assigned"},
    {"cpus", cpu_list}, {"ninja", jobs}, {"cargo", jobs}, {"firefox", jobs}
   };
   append_trace(record);
  }
  try {
   traced("bootstrap", [&] {
    acquire_mutation();
    ensure_docker();
    cache_layout();
   });
   prepare_producers();
   producers_prepared = true;
   builder_ready = true;
   default_builder_ready = true;
   active_container_names = {release_container};
   docker({"rm", "-f", release_container}, {.quiet = true});
   task([&] {
    traced("release_ninja", [&] {
     ensure_build_image();
     firefox_build_input();
     quiet_log("building the unified Release Ninja graph with " + std::to_string(jobs) + " workers on CPUs " + cpu_list);
     const auto result = toolchain_command(std::string(commands::release_ninja), release_container);
     if (result.status) throw Exit{result.status};
    });
   });
   task([&] { traced("runtime_base", [&] { ensure_runtime_base(); }); });
   wait_tasks();
   traced("package", [&] { stage_release(); });
   release_mutation();
   trace_event("build", "finish", "success");
  } catch (const Exit& error) {
   trace_event("build", "finish", "failure:" + std::to_string(error.status));
   throw;
  }
 }
 void generation(bool application) {
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  if (!application) firefox_build_input();
  quiet_log(application ? "generating typed application bindings" : "generating Protocol-17 cross-language bindings and fixtures");
  must_toolchain(release_container + (application ? "-application-bindings-generation" : "-protocol-generation"), application ? commands::bindings : commands::protocol);
  release_mutation();
 }
 void gui_build() {
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  quiet_log("building iced Release bundle into " + cmake + "/gui/frontend/iced/dist");
  must_toolchain(gui_container, commands::gui_build);
  release_mutation();
 }
 void lock_update(bool firefox_owner) {
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  if (firefox_owner)
   log("refreshing the Firefox dependency lock from vendored sources");
  else
   quiet_log("updating iced dependency lock for the native browser test graph");
  must_toolchain(firefox_owner ? release_container + "-lock" : gui_container, firefox_owner ? commands::firefox_lock : commands::gui_lock);
  release_mutation();
 }
 void run_tidy() {
  prepare_logging(env("MMLTK_TIDY_LOG_FILE"));
  setenv("MMLTK_TIDY_LOG_FILE", operation_log.c_str(), 1);
  setenv("MMLTK_TIDY_TEE_STARTED", "1", 1);
  acquire_mutation();
  Args common_options, tool_files, product_files;
  std::string resume;
  bool files = false, only_product_resume = false;
  auto relative = [&](std::string path) {
   if (path.starts_with("/host")) path.erase(0, 5);
   if (path.starts_with(repo + "/")) path.erase(0, repo.size() + 1);
   return path;
  };
  std::optional<std::set<std::string>> registered_tools;
  auto standalone_source = [&](const std::string& path) {
   if (!path.starts_with("tools/")) return false;
   if (!registered_tools) {
    const auto tools_lock = lock_file(repository_cache + "/locks/tools.lock", LOCK_EX);
    registered_tools.emplace();
    const auto database = cache + "/cmake/tools/compile_commands.json";
    require(fs::is_regular_file(database), "tools compile database is missing; run ./mmltk --build-tools");
    for (const auto& entry : support::json::parse(read_text(database))) {
     auto file = entry.at("file").get<std::string>();
     if (file.starts_with("/workspace/"))
      file.erase(0, 11);
     else
      file = relative(std::move(file));
     registered_tools->insert(std::move(file));
    }
   }
   return registered_tools->contains(path);
  };
  for (std::size_t index = 0; index < rewritten.size(); ++index) {
   const auto& argument = rewritten[index];
   if (argument == "--file") {
    require(!files && resume.empty(), "--file may occur once and cannot be combined with --start-at");
    files = true;
    require(index + 1 < rewritten.size() && !starts(rewritten[index + 1], "--"), "--file requires a source path");
    while (index + 1 < rewritten.size() && !starts(rewritten[index + 1], "--")) {
     const auto path = relative(rewritten[++index]);
     (standalone_source(path) ? tool_files : product_files).push_back(path);
    }
   } else if (argument == "--start-at") {
    require(!files && resume.empty(), "--start-at may occur once and cannot be combined with --file");
    require(index + 1 < rewritten.size() && !starts(rewritten[index + 1], "--"), "--start-at requires a file path");
    resume = relative(rewritten[++index]);
    only_product_resume = !standalone_source(resume);
   } else
    common_options.push_back(argument);
  }
  require(!tidy_tools || (!only_product_resume && product_files.empty()),
   "--tidy-tools selects only the standalone driver graph; use --tidy for product-owned units and the oracle/Wayland image build for their isolated units");
  const bool rust_only = std::find(common_options.begin(), common_options.end(), "--rust-only") != common_options.end();
  require(!tidy_tools || !rust_only, "--tidy-tools does not select Rust sources");
  require(!tidy_tools || !third_party, "--tidy-tools does not select third_party sources");
  auto selection_options = [&](const Args& selected, bool tools) {
   Args result = common_options;
   if (!selected.empty()) {
    result.push_back("--file");
    append(result, selected);
   }
   if (!resume.empty() && tools != only_product_resume) append(result, {"--start-at", resume});
   return result;
  };
  const auto tool_options = selection_options(tool_files, true), product_options = selection_options(product_files, false);
  if (!rust_only && !only_product_resume && (!files || !tool_files.empty())) {
   must({"bash", repo + "/tools/bootstrap/bootstrap_mmltk.sh", "--image", repo}, {.environment = {{"MMLTK_BOOTSTRAP_CAPTURED", "1"}}});
   tools_stage_lock = lock_file(repository_cache + "/locks/tools.lock", LOCK_EX);
   owned_dirs({cache, cmake, cmake + "/tools"});
   Args args{
    "run", "--rm", "--init", "-i", "--name", tidy_container, "--user", user(), "--volume", repo + ":/workspace:rw", "--workdir", "/workspace", "--env", "HOME=/tmp", "--env",
    "MMLTK_REPO_ROOT=/workspace", "--env", "MMLTK_TIDY_BUILD_DIR=" + container_cache + "/cmake/tools", "--env", "MMLTK_TIDY_LOG_FILE=" + operation_log, "--env", "MMLTK_TIDY_TEE_STARTED=1",
    "--entrypoint", "/workspace/build/tools/mmltk"
   };
   labels(args, mode);
   diagnostic_tools(args);
   args.push_back(tools_image);
   append(args, tail(tool_arguments(Command::run_static_analysis, {"--tools-only"})));
   append(args, tool_options);
   owned_docker_must(tidy_container, args);
   tools_stage_lock.reset();
  }
  if (tidy_tools || (files && product_files.empty())) {
   release_mutation();
   return;
  }
  ensure_docker();
  ensure_analysis();
  compiler_cache_dirs();
  common_groups();
  Args args{"run", "--rm", "--init", "-i", "--name", tidy_container, "--security-opt", "seccomp=unconfined"};
  labels(args, mode);
  if (isatty(0) && isatty(1)) args.push_back("-t");
  append(args, {"--user", user()});
  for (const auto& value : Args{
        "HOME=/host" + cache + "/container-home", "CARGO_HOME=/host" + cargo + "/home", "CARGO_TARGET_DIR=/host" + cargo + "/target/browser-app", "CARGO_BUILD_JOBS=" + std::to_string(jobs),
        "CCACHE_BASEDIR=/host" + repo, "CCACHE_DIR=/host" + ccache, "CCACHE_TEMPDIR=/host" + ccache + "/tmp", "CCACHE_NOHASHDIR=1", "CCACHE_MAXSIZE=50G", "CCACHE_SLOPPINESS=pch_defines,time_macros",
        "MMLTK_REPO_ROOT=/host" + repo, "MMLTK_TIDY_BUILD_DIR=/host" + cmake + "/analysis", "MMLTK_TIDY_LOG_FILE=/host" + operation_log, "MMLTK_TIDY_TEE_STARTED=1"
       })
   append(args, {"--env", value});
  append(args, {"--volume", "/:/host:rw", "--workdir", "/host" + host_workdir});
  append(args, groups);
  append(args, {"--entrypoint", "/host" + repo + "/build/tools/mmltk", analysis_image});
  append(args, tail(tool_arguments(Command::run_static_analysis)));
  if (third_party) args.push_back("--third_party");
  append(args, product_options);
  owned_docker_must(tidy_container, args);
  release_mutation();
 }
 int declaration_tool(std::string operation, Args arguments) {
  Command selected = Command::generate_cleanup_json;
  std::string configure;
  Args args{"--raw-cpd"};
  if (operation == "format") {
   selected = Command::format_declarations;
   args.clear();
  } else if (operation == "includes") {
   selected = Command::audit_includes;
   args.clear();
   if (first(arguments) == "--refresh") {
    arguments = tail(arguments);
    configure = tool_shell(Command::configure_cmake_graph) +
                " release \"${MMLTK_CONTAINER_CACHE_ROOT}/cmake/release\" -DBUILD_MMLTK_BROWSER_HOST=ON -DBUILD_MMLTK_FIREFOX_RUNTIME=ON -DMMLTK_BUILD_JOBS=\"${MMLTK_JOBS}\" "
                "-DMMLTK_FIREFOX_CACHE_ROOT=\"${MMLTK_FIREFOX_CACHE_ROOT}\"; ";
   }
  }
  append(args, arguments);
  args = tool_arguments(selected, args);
  acquire_mutation();
  if (operation == "includes") owned_dirs({repo + "/output"});
  ensure_docker();
  cache_layout();
  ensure_build_image();
  const int status = owned_toolchain(cleanup_container, "set -euo pipefail; " + configure + quoted(args));
  release_mutation();
  return status;
 }
 int cleanup_tool_tests(const Args& arguments) {
  require(arguments.empty() || (arguments.size() == 2 && arguments[0] == "--test-name-pattern" && !arguments[1].empty()), "cleanup-tool accepts only --test-name-pattern PATTERN");
  std::string command = "set -uo pipefail; status=0; ";
  for (auto selected : {Command::test_generate_cleanup_json, Command::test_format_declarations, Command::test_review_candidates, Command::test_audit_includes})
   command += quoted(tool_arguments(selected, arguments)) + " || status=$?; ";
  command += "exit ${status}";
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  quiet_log("running cleanup and declaration tooling fixture tests");
  const int status = owned_toolchain(test_run_container, command);
  release_mutation();
  return status;
 }
 void cleanup_reports(const std::string& profile) {
  require(profile == "cpp" || profile == "frontend" || profile == "tools" || profile == "all", "--cleanup-report requires cpp, frontend, tools, or all");
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  for (const auto& selector : profile == "all" ? Args{"cpp", "frontend", "tools"} : Args{profile}) {
   quiet_log("generating " + selector + " cleanup report");
   must_toolchain(cleanup_container, "set -euo pipefail\n" + tool_shell(Command::generate_cleanup_json) + " --" + selector);
  }
  release_mutation();
 }
 std::string existing_diagnostic_image(const std::string& image, const std::string& daemon_message, const std::string& missing_message = {}) {
  require(command_exists("docker"), "docker is not installed");
  require(server_ready(), daemon_message);
  auto result = docker({"image", "inspect", image, "--format", "{{.Id}}"}, {.capture = true, .stderr_quiet = true});
  require(!result.status, missing_message.empty() ? "diagnostic requires existing image '" + image + "'" : missing_message);
  return trim(result.out);
 }
 int log_tool(bool fixture, const Args& arguments) {
  require(repo.find(':') == std::string::npos, "--logs repository path cannot contain ':'");
  const bool tools_ownership = fixture && arguments == Args{"--tools-ownership"};
  const auto id =
   existing_diagnostic_image(build_image, "docker daemon is unavailable; log tooling did not start", "log tooling requires existing image '" + build_image + "'; no image was built or pulled");
  const auto name = container_name + "-logs";
  Args args{
   "run", "--rm", "--init", "--name", name, "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--user", tools_ownership ? "0:0" : user(), "--env",
   "PYTHONDONTWRITEBYTECODE=1", "--env", "MMLTK_LOG_HOST_ROOT=" + repo, "--volume", repo + ":/workspace:ro", "--workdir", "/workspace", "--entrypoint", "/workspace/build/tools/mmltk"
  };
  labels(args, mode);
  if (fixture) {
   append(args, {"--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=32m"});
   if (tools_ownership)
    for (const auto* capability : {"CHOWN", "DAC_OVERRIDE", "FOWNER", "SETUID", "SETGID", "KILL"}) append(args, {"--cap-add", capability});
   for (const auto& item : arguments)
    require(tools_ownership || item == "--list-tests" || matches(item, "[A-Za-z_][A-Za-z0-9_]*\\.[A-Za-z_][A-Za-z0-9_]*"),
     "--test log-query-tool expects Class.test_method selectors or the standalone --tools-ownership option");
  }
  args.push_back(id);
  auto invocation = tool_arguments(fixture ? Command::test_log_query : Command::log_query, arguments);
  append(args, tail(invocation));
  return owned_docker(name, args).status;
 }
 int gpu_environment(const std::string& selected) {
  if (selected == "--help" || selected == "-h") {
   std::cout << commands::gpu_environment_help;
   return 0;
  }
  require(selected == "runtime" || selected == "wayland-validation" || selected == "development", "--diagnose-gpu-environment accepts runtime, wayland-validation, or development");
  const auto image = selected == "runtime" ? image_name : selected == "wayland-validation" ? validation_image : build_image;
  require(repo.find(':') == std::string::npos, "--diagnose-gpu-environment repository path cannot contain ':'");
  const auto id = existing_diagnostic_image(
              image, "docker daemon is unavailable; GPU environment diagnostic did not start", "diagnostic requires existing image '" + image + "'; no image was built or pulled"),
             name = container_name + "-gpu-environment";
  Args args{
   "run", "--rm", "--init", "--pull", "never", "--name", name, "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--gpus", "all", "--user", user(),
   "--env", "NVIDIA_DRIVER_CAPABILITIES=all", "--env", "MMLTK_DIAGNOSTIC_IMAGE=" + image, "--env", "MMLTK_DIAGNOSTIC_IMAGE_ID=" + id, "--entrypoint", "/opt/mmltk-tools/mmltk"
  };
  common_groups();
  labels(args, "diagnose-gpu-environment");
  append(args, groups);
  diagnostic_tools(args);
  args.push_back(id);
  append(args, tail(tool_arguments(Command::diagnose_gpu_environment)));
  return owned_docker(name, args).status;
 }
 int io_diagnostic(const std::string& input) {
  std::string path;
  try {
   path = existing(input);
  } catch (...) { die("compiled file does not exist: " + input); }
  require(fs::is_regular_file(path) && access(path.c_str(), R_OK) == 0, "--diagnose-io requires a readable regular compiled file");
  require(path.find(':') == std::string::npos, "--diagnose-io file path cannot contain ':'");
  const auto id =
   existing_diagnostic_image(build_image, "docker daemon is unavailable; diagnostic did not start", "diagnostic requires existing image '" + build_image + "'; no image was built or pulled");
  quiet_log("read-only I/O capability report in " + build_image + "; no dataset reads or benchmarks");
  Args args{
   "run", "--rm", "--init", "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--gpus", "all", "--user", user(), "--env",
   "NVIDIA_DRIVER_CAPABILITIES=utility", "--env", "PYTHONDONTWRITEBYTECODE=1", "--env", "MMLTK_DIAGNOSTIC_IMAGE=" + build_image, "--env", "MMLTK_DIAGNOSTIC_IMAGE_ID=" + id, "--volume",
   path + ":/diagnostic/dataset:ro", "--entrypoint", "/opt/mmltk-tools/mmltk"
  };
  common_groups();
  append(args, groups);
  diagnostic_tools(args);
  args.push_back(id);
  append(args, tail(tool_arguments(Command::diagnose_dataset_io, {"/diagnostic/dataset", "--source-path", path})));
  return docker(args).status;
 }
 int nvidia_diagnostic(const Args& arguments) {
  const auto source = first(arguments);
  require(source == "donor" || source == "development", "--diagnose-nvidia-payload requires donor or development, followed by public header filenames");
  const auto headers = tail(arguments);
  for (const auto& header : headers) require(matches(header, "[A-Za-z0-9_.-]+"), "NVIDIA diagnostic headers must be public filenames, without directory paths");
  const auto image = source == "donor" ? manifest_value("donor") : build_image;
  if (source == "donor") {
   const auto output = repo + "/build/diagnostics/nvidia-payload";
   acquire_mutation();
   ensure_docker();
   ensure_builder();
   owned_dirs({output});
   docker_must({"buildx", "build", "--builder", builder, "--progress=plain", "--file", repo + "/docker/Dockerfile.nvidia-diagnostic", "--build-arg", "MMLTK_NVIDIA_DONOR_IMAGE=" + image,
                "--build-context", "mmltk_tools=" + repo + "/build/tools", "--build-arg", "MMLTK_DIAGNOSTIC_HEADERS=" + joined(headers, ","), "--output", "type=local,dest=" + output, repo},
    {.to_stderr = true});
   std::cout << read_text(output + "/headers.json");
   release_mutation();
   return 0;
  }
  const auto id = existing_diagnostic_image(image, "docker daemon is unavailable; diagnostic did not start", "diagnostic requires existing image '" + image + "'; no image was built or pulled");
  quiet_log("read-only NVIDIA header inventory in " + image);
  Args args{
   "run", "--rm", "--init", "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--user", user(), "--env", "PYTHONDONTWRITEBYTECODE=1", "--env",
   "MMLTK_DIAGNOSTIC_IMAGE=" + image, "--env", "MMLTK_DIAGNOSTIC_IMAGE_ID=" + id, "--volume", repo + "/docker:/diagnostic_metadata:ro", "--entrypoint", "/opt/mmltk-tools/mmltk"
  };
  diagnostic_tools(args);
  args.push_back(id);
  append(args, tail(tool_arguments(Command::diagnose_nvidia_payload, headers)));
  return docker(args).status;
 }
 std::string artifact_directory(const std::string& name) {
  owned_dirs({repo + "/build/diagnostics"});
  std::string path = repo + "/build/diagnostics/" + name + ".XXXXXX";
  require(mkdtemp(path.data()) != nullptr, "cannot create diagnostic artifact directory");
  require(chown(path.c_str(), uid, gid) == 0, "cannot set diagnostic artifact ownership");
  return path;
 }
 Args diagnostic_base(const std::string& name, const std::string& tmp_size = "64m") {
  Args args{
   "run", "--rm", "--init", "--pull", "never", "--name", name, "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--user", user(), "--workdir",
   "/workspace", "--tmpfs", "/tmp:rw,nosuid,nodev,size=" + tmp_size, "--volume", repo + ":/workspace:ro", "--entrypoint", "/usr/bin/timeout"
  };
  labels(args, mode);
  return args;
 }
 int native_diagnostic(bool link, const Args& arguments) {
  if (first(arguments) == "--help" || first(arguments) == "-h") {
   std::cout << (link ? commands::link_help : commands::symbols_help);
   return 0;
  }
  require(!arguments.empty(), link ? "--diagnose-native-link requires one native target" : "--diagnose-native-symbols requires repository-relative artifacts");
  require(repo.find(':') == std::string::npos, "diagnostic repository path cannot contain ':'");
  const auto id = existing_diagnostic_image(build_image, link ? "docker daemon is unavailable; link diagnostic did not start" : "docker daemon is unavailable; symbol diagnostic did not start");
  const auto name = container_name + (link ? "-native-link" : "-native-symbols");
  auto args = diagnostic_base(name, link ? "1g" : "64m");
  if (link) {
   const auto artifacts = artifact_directory("native-link");
   append(args, {"--volume", artifacts + ":/diagnostic/output:rw", "--volume", repo + ":/host" + repo + ":ro"});
   log("native link diagnostic artifacts: " + artifacts);
  }
  append(args, {id, "--signal=KILL", "120s"});
  append(args, tool_arguments(link ? Command::diagnose_native_link : Command::diagnose_native_symbols));
  append(args, arguments);
  return owned_docker(name, args).status;
 }
 int process_diagnostic(Args arguments) {
  if (first(arguments) == "--help" || first(arguments) == "-h") {
   std::cout << commands::process_help;
   return 0;
  }
  std::string selected_mode, action, executable;
  Args selector;
  if (!arguments.empty() && !starts(arguments[0], "--")) {
   selected_mode = arguments[0];
   arguments = tail(arguments);
  }
  if (!arguments.empty()) {
   require(arguments.size() >= 2, "process action requires one executable name");
   action = arguments[0];
   executable = arguments[1];
   require(action == "--backtrace" || action == "--terminate" || action == "--read-file", "unknown process diagnostic action '" + action + "'");
   require(executable == "mmltk" || matches(executable, "mmltk[_-][A-Za-z0-9_-]+"), "expected an exact mmltk native executable name");
   for (std::size_t i = 2; i < arguments.size(); i += 2) {
    require(i + 1 < arguments.size(), "process diagnostic option requires a value");
    const auto& option = arguments[i];
    const auto& value = arguments[i + 1];
    if (option == "--pid")
     require(matches(value, "[1-9][0-9]*"), "expected a positive container PID");
    else if (option == "--file")
     require(action == "--read-file" && starts(value, "/"), "--file requires --read-file and an absolute container path");
    else
     die("unknown process diagnostic option '" + option + "'");
    append(selector, {option, value});
   }
  }
  require(command_exists("docker"), "docker is not installed");
  Args args{"docker", "ps", "--filter", "label=com.mmltk.wrapper=1", "--filter", "label=com.mmltk.repo=" + repo};
  if (!selected_mode.empty()) append(args, {"--filter", "label=com.mmltk.mode=" + selected_mode});
  append(args, {"--format", "{{.ID}}"});
  const auto result = capture(timeout(args, 15));
  require(!result.status, "could not enumerate running wrapper containers");
  const auto containers = words(result.out);
  if (containers.empty()) {
   require(action.empty(), "no running wrapper container for process action");
   std::cout << "No matching running wrapper containers.\n";
   return 0;
  }
  if (!action.empty()) {
   require(containers.size() == 1, "process action requires exactly one running wrapper container; select WRAPPER_MODE");
   args = {"docker", "exec", "-i", "--user", action == "--backtrace" ? "0" : user()};
   if (action == "--backtrace") args.push_back("--privileged");
   args.push_back(containers[0]);
   append(args, tool_arguments(Command::diagnose_native_process, {action, executable}, "/opt/mmltk-tools/mmltk"));
   append(args, selector);
   return run(timeout(args, 45)).status;
  }
  for (const auto& id : containers) {
   require(
    !run(timeout({"docker", "inspect", "--format", "Container {{.Name}} ({{.Id}}), mode={{index .Config.Labels \"com.mmltk.mode\"}}", id}, 15)).status, "could not inspect wrapper container " + id);
   require(!run(timeout({"docker", "top", id, "-eo", "pid,ppid,etime,time,pcpu,pmem,stat,wchan:32,comm"}, 15)).status, "could not inspect processes in wrapper container " + id);
  }
  return 0;
 }
 int gpu_executable(const std::string& executable_path, const Args& arguments) {
  auto result = docker({"image", "inspect", validation_image, "--format", "{{.Id}}"}, {.capture = true, .stderr_quiet = true});
  require(!result.status, "diagnostic requires existing image '" + validation_image + "'; run ./mmltk --build first");
  const auto id = trim(result.out), name = container_name + "-gpu-program";
  require(executable_path.find(':') == std::string::npos, "diagnostic executable path cannot contain ':'");
  Args args{
   "run", "--rm", "--init", "--pull", "never", "--name", name, "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--gpus", "all", "--user", user(),
   "--env", "NVIDIA_DRIVER_CAPABILITIES=all", "--tmpfs", "/tmp:rw,nosuid,nodev,size=64m", "--volume", executable_path + ":/diagnostic/probe:ro", "--entrypoint", "/usr/bin/timeout"
  };
  common_groups();
  labels(args, "diagnose-gpu-program");
  append(args, groups);
  log("standalone diagnostic runtime: " + id);
  append(args, {id, "--signal=KILL", "120s", "/diagnostic/probe"});
  append(args, arguments);
  return owned_docker(name, args).status;
 }
 int gpu_program(Args arguments) {
  if (first(arguments) == "--help" || first(arguments) == "-h") {
   std::cout << commands::gpu_program_help;
   return 0;
  }
  bool torch = first(arguments) == "--torch";
  if (torch) arguments = tail(arguments);
  require(!arguments.empty(), "--diagnose-gpu-program requires a C++ source path");
  std::string source;
  try {
   source = existing(arguments[0]);
  } catch (...) { die("diagnostic source does not exist: " + arguments[0]); }
  arguments = tail(arguments);
  require(fs::is_regular_file(source) && access(source.c_str(), R_OK) == 0 && source.ends_with(".cpp"), "--diagnose-gpu-program requires a readable .cpp file");
  require(source.find(':') == std::string::npos, "diagnostic source path cannot contain ':'");
  const auto id = existing_diagnostic_image(build_image, "docker daemon is unavailable; diagnostic did not start");
  require(docker_ok({"image", "inspect", validation_image}), "diagnostic requires existing image '" + validation_image + "'");
  const auto artifacts = artifact_directory("gpu-program"), name = container_name + "-gpu-program-build";
  Args args{
   "run", "--rm", "--init", "--pull", "never", "--name", name, "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", "none", "--user", user(), "--tmpfs",
   "/tmp:rw,nosuid,nodev,size=256m", "--volume", parent(source) + ":/diagnostic/source:ro", "--volume", artifacts + ":/diagnostic/output:rw", "--env", "MMLTK_DIAGNOSTIC_SOURCE=" + filename(source),
   "--env", std::string("MMLTK_DIAGNOSTIC_TORCH=") + (torch ? "1" : "0"), "--entrypoint", "/usr/bin/timeout"
  };
  labels(args, "diagnose-gpu-program");
  log("standalone diagnostic artifacts: " + artifacts);
  append(args, {id, "--signal=KILL", "120s", "/bin/bash", "-c", std::string(commands::gpu_compile)});
  owned_docker_must(name, args);
  return gpu_executable(artifacts + "/probe", arguments);
 }
 int benchmark_image(const Args& arguments) {
  require(repo.find(':') == std::string::npos, "diagnostic repository path cannot contain ':'");
  const auto id = existing_diagnostic_image(build_image, "docker daemon is unavailable; image diagnostic did not start"), name = container_name + "-benchmark-image";
  owned_dirs({repo + "/build/validation/benchmark-image"});
  auto args = diagnostic_base(name);
  append(args, {"--security-opt", "seccomp=unconfined", "--ulimit", "nice=40:40", "--volume", repo + "/build/validation/benchmark-image:/evidence:rw"});
  auto query = args;
  append(query, {"--env", "MMLTK_BENCHMARK_NATIVE_REQUIREMENTS=1", id, "--signal=KILL", "600s"});
  append(query, tool_arguments(Command::diagnose_benchmark_image));
  append(query, arguments);
  const auto requirements = owned_docker(name, query, {.capture = true});
  if (requirements.status) return requirements.status;
  Args targets;
  const auto requirement = mmltk::tools::text::trim_newlines(requirements.out);
  if (requirement == "native:compiled")
   targets = {"mmltk_diagnose_compiled_sample"};
  else if (requirement == "native:mask")
   targets = {"mmltk_diagnose_coconut_mask"};
  else if (requirement == "native:compiled,mask")
   targets = {"mmltk_diagnose_compiled_sample", "mmltk_diagnose_coconut_mask"};
  else if (requirement != "native:") {
   std::cout << requirement << '\n';
   return 0;
  }
  if (!targets.empty()) {
   acquire_mutation();
   cache_layout();
   const auto old_image = build_image;
   build_image = id;
   const std::string command =
    "set -euo pipefail\n" + tool_shell(Command::configure_cmake_graph) +
    " release \"${MMLTK_CONTAINER_CACHE_ROOT}/cmake/benchmark-image-release\" -DBUILD_MMLTK_BROWSER_HOST=OFF -DBUILD_MMLTK_FIREFOX_RUNTIME=OFF -DMMLTK_BUILD_JOBS=\"${MMLTK_JOBS}\" "
    "-DMMLTK_FIREFOX_CACHE_ROOT=\"${MMLTK_FIREFOX_CACHE_ROOT}\"\nfor target in " +
    joined(targets) + "; do timeout --signal=KILL 120s cmake --build \"${MMLTK_CONTAINER_CACHE_ROOT}/cmake/benchmark-image-release\" --parallel \"${MMLTK_JOBS}\" --target \"${target}\"; done";
   const auto status = owned_toolchain(name + "-build", command, {}, {.to_stderr = true});
   build_image = old_image;
   if (status) {
    log("benchmark image diagnostic target preparation failed (status=" + std::to_string(status) + ")");
    release_mutation();
    return status;
   }
   append(args, {"--env-file", cmake + "/benchmark-image-release/benchmark-image-diagnostics.env"});
  }
  append(args, {id, "--signal=KILL", "600s"});
  append(args, tool_arguments(Command::diagnose_benchmark_image));
  append(args, arguments);
  const auto status = owned_docker(name, args).status;
  release_mutation();
  return status;
 }
 std::pair<std::string, std::string> ensure_oracle_image(const std::string& revision) {
  const auto input = fingerprint({revision, donor, trim(read_optional(repo + "/build/tools/.inputs.sha256"))},
              {"docker/Dockerfile.rfdetr-oracle", "docker/nvidia-payload.json", "tools/oracle/rfdetr_oracle_requirements.txt", "tools/oracle/generate_rfdetr_upstream_oracle.cpp", "tools/support/cpp_support.cpp",
               "tools/support/cpp_text.cpp", "tools/support/cpp_digest.cpp", "tools/CMakeLists.txt", "tools/cmake/Toolchain.cmake", "tools/cmake/RuntimeClosure.cmake", "tools/cmake/StdModules.cmake"}),
             image = image_name + "-rfdetr-oracle";
  must({"bash", repo + "/tools/bootstrap/bootstrap_mmltk.sh", "--image", repo}, {.environment = {{"MMLTK_BOOTSTRAP_CAPTURED", "1"}}});
  const auto tools_id = image_id(tools_image), tools_alias = image_alias("tools", tools_id);
  task([&] {
   cached_image("default", "-", image, repo + "/docker/Dockerfile.rfdetr-oracle", "rfdetr-oracle", input, repo,
    {"--platform", manifest_value("platform"), "--build-arg", "MMLTK_NVIDIA_DONOR_IMAGE=" + donor, "--build-context", "mmltk_tools_build=docker-image://" + tools_alias});
  });
  if (const int image_status = wait_one_task(); image_status) throw Exit{image_status};
  return {input, image};
 }
 int oracle(Args arguments) {
  if (first(arguments) == "--help" || first(arguments) == "-h") {
   std::cout << commands::oracle_help;
   return 0;
  }
  const std::string revision = "e9a138f70cc14e3fddca029a0683055e34ab8f35";
  if (arguments == Args{"--build-only"}) {
   mode = "build-rfdetr-oracle";
   prepare_logging(env("MMLTK_BUILD_LOG_FILE"));
   acquire_mutation();
   ensure_docker();
   cache_layout();
   nvidia_inputs();
   ensure_oracle_image(revision);
   release_mutation();
   return 0;
  }
  std::string upstream, output, sdpa = "default";
  for (std::size_t i = 0; i < arguments.size(); i += 2) {
   const auto& option = arguments[i];
   if (option == "--upstream" || option == "--output") {
    require(i + 1 < arguments.size() && !arguments[i + 1].empty(), option + " requires a path");
    (option == "--upstream" ? upstream : output) = arguments[i + 1];
   } else if (option == "--sdpa") {
    require(i + 1 < arguments.size() && (arguments[i + 1] == "default" || arguments[i + 1] == "math"), "--sdpa requires default or math");
    sdpa = arguments[i + 1];
   } else
    die("unknown RF-DETR oracle option '" + option + "'");
  }
  require(!upstream.empty() && !output.empty(), "RF-DETR oracle requires --upstream and --output");
  upstream = existing(upstream);
  output = normalized(output);
  require(upstream.find(':') == std::string::npos && output.find(':') == std::string::npos, "oracle mount paths cannot contain colons");
  require(fs::is_regular_file(upstream + "/src/rfdetr/models/lwdetr.py"), "oracle source must be the RF-DETR checkout root");
  const Args git_prefix{"git", "--no-optional-locks", "-c", "safe.directory=" + upstream, "-C", upstream};
  auto source_git = [&](const Args& command, const std::string& message) {
   auto args = git_prefix;
   append(args, command);
   const auto result = capture(args);
   require(!result.status, message);
   return mmltk::tools::text::trim_newlines(result.out);
  };
  require(source_git({"rev-parse", "HEAD"}, "cannot read oracle upstream revision") == revision, "oracle requires pinned upstream " + revision);
  require(source_git({"status", "--porcelain", "--untracked-files=all"}, "cannot inspect oracle upstream worktree").empty(), "oracle upstream checkout must be clean, including untracked files");
  const auto git_dir = source_git({"rev-parse", "--absolute-git-dir"}, "cannot resolve oracle upstream Git directory"),
             common_dir = source_git({"rev-parse", "--path-format=absolute", "--git-common-dir"}, "cannot resolve oracle upstream common Git directory"),
             tree = source_git({"rev-parse", "HEAD^{tree}"}, "cannot read oracle upstream tree");
  require(git_dir.find(':') == std::string::npos && common_dir.find(':') == std::string::npos, "oracle Git mount paths cannot contain colons");
  require(!within(output, upstream), "oracle output must be outside the read-only upstream checkout");
  require(!present(output + "/manifest.json") && !present(output + "/verdict.json") && !present(output + "/references"), "oracle output already contains evidence; choose a fresh directory");
  const auto weights = repository_cache + "/mmltk/weights";
  require(fs::is_regular_file(weights + "/rf-detr-nano.pth") && fs::is_regular_file(weights + "/rf-detr-seg-nano.pt"),
   "oracle requires the cached official rf-detr-nano.pth and rf-detr-seg-nano.pt artifacts");
  owned_dirs({output});
  prepare_logging(output + "/wrapper.log");
  acquire_mutation();
  ensure_docker();
  cache_layout();
  nvidia_inputs();
  const auto [image_input, image] = ensure_oracle_image(revision);
  const auto input = sha(image_input + "\n" + tree + "\n" + sdpa), name = container_name + "-upstream";
  Args args{
   "run", "--rm", "--init", "--name", name, "--gpus", "all", "--ipc", "host", "--network", "none", "--read-only", "--user", user(), "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
   "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=4g", "--env", "HOME=/tmp", "--env", "NVIDIA_DRIVER_CAPABILITIES=compute,utility", "--env", "GIT_DIR=/upstream-git", "--env",
   "GIT_COMMON_DIR=/upstream-common", "--env", "GIT_WORK_TREE=/upstream", "--env", "GIT_OPTIONAL_LOCKS=0", "--env", "HF_HOME=/tmp/huggingface", "--env", "TRITON_CACHE_DIR=/tmp/triton", "--volume",
   upstream + ":/upstream:ro", "--volume", weights + ":/weights:ro", "--volume", git_dir + ":/upstream-git:ro", "--volume", common_dir + ":/upstream-common:ro", "--volume", output + ":/evidence:rw",
   "--workdir", "/tmp", "--entrypoint", "/usr/bin/timeout"
  };
  execution_policy(args);
  labels(args, mode);
  diagnostic_tools(args);
  append(args, {image, "--signal=TERM", "--kill-after=15s", "7200s", "/opt/rfdetr-oracle/mmltk-rfdetr-upstream-oracle", "--upstream", "/upstream", "--output", "/evidence", "--weights", "/weights",
                "--fingerprint", input, "--sdpa", sdpa});
  const int status = owned_docker(name, args).status;
  release_mutation();
  if (status) {
   log("upstream reference production failed (exit " + std::to_string(status) + "); evidence retained in " + output);
   return status;
  }
  int failure = 0;
  for (const auto* target : {"mmltk_backend_models_rfdetr_core_tests", "mmltk_backend_models_rfdetr_training_tests"}) {
   // Re-enter this native wrapper, with fresh ownership and transcript state.
   const Args invocation{self, "--test", "rfdetr", "--executable", target, "--env", "MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=" + host_path(output), "--", "[upstream_oracle]", "--warn", "UnmatchedTestSpec"};
   task([&, invocation, target] {
    support::RunOptions options;
    options.env["MMLTK_TEST_LOG_FILE"] = output + "/" + target + ".log";
    support::exec(invocation, options);
   });
   const int comparison_status = wait_one_task();
   if (!failure) failure = comparison_status;
  }
  if (failure) {
   log("native oracle comparison failed (exit " + std::to_string(failure) + "); reference artifacts and comparisons retained in " + output);
   return failure;
  }
  support::json verdict{
   {"status", "passed_required_checks"}, {"evidence", "actual upstream matched-state forward/objectives and complete derivative vectors; identical-gradient AdamW; native Match-Free compilation"},
   {"measurements", "per-tensor bounds, autonomous trajectories and stock AMP matcher differences retained; no accuracy or trajectory-equality claim"}, {"fingerprint", input},
   {"manifest_sha256", get({"sha256sum", output + "/manifest.json"}).substr(0, 64)}
  };
  write_text(output + "/.verdict.partial", verdict.dump() + "\n");
  fs::rename(output + "/.verdict.partial", output + "/verdict.json");
  log("upstream production and required matched-state comparisons passed; trajectory differences remain measurements; " + output + "/verdict.json");
  return 0;
 }
 void ensure_validation_image() {
  const auto runtime_result = docker({"image", "inspect", image_name, "--format", "{{.Id}}"}, {.capture = true});
  require(!runtime_result.status, "headless compositor requires the packaged runtime image; run ./mmltk --build first");
  const auto runtime_id = trim(runtime_result.out), runtime_alias = image_alias("wayland-runtime", runtime_id);
  const auto build_result = docker({"image", "inspect", build_image, "--format", "{{.Id}}"}, {.capture = true});
  require(!build_result.status, "headless compositor requires the GCC 16.2 build image; run ./mmltk --build first");
  const auto build_id = trim(build_result.out), build_alias = image_alias("wayland-build", build_id);
  cached_image("default", "-", validation_image, repo + "/docker/Dockerfile.wayland-validation", "wayland-validation",
   fingerprint({"wayland-validation", runtime_id, build_id},
    {"docker/Dockerfile.wayland-validation", "tools/wayland/headless_weston_seat.cpp", "tools/CMakeLists.txt", "tools/cmake/Toolchain.cmake", "tools/cmake/RuntimeClosure.cmake", "tools/cmake/StdModules.cmake"}),
   repo,
   {"--build-context", "mmltk_runtime=docker-image://" + runtime_alias, "--build-context", "mmltk_build=docker-image://" + build_alias, "--build-arg", "MMLTK_RUNTIME_IDENTITY=" + runtime_id,
    "--build-arg", "MMLTK_BUILD_IDENTITY=" + build_id});
  verify_alias(runtime_alias, runtime_id);
  verify_alias(build_alias, build_id);
  verify_label(validation_image, "runtime-identity", runtime_id);
  verify_label(validation_image, "build-identity", build_id);
 }
 void explicit_test_environment(Args& args) {
  for (const auto& entry : test_environment) {
   const auto name = entry.substr(0, entry.find('='));
   if (name != "MMLTK_LOG_LEVEL" && name != "MMLTK_LOG_FILE" && name != "MMLTK_LOG_DIR") append(args, {"--env", entry});
  }
 }
 void forwarded_test_environment(Args& args) {
  native_environment(args);
  for (const auto* name :
   {"MMLTK_RUN_WORKSPACE_VULKAN_TEST", "MMLTK_RUN_EXPLORE_CUDA_TEST", "MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION", "MMLTK_GDR_TEST_DEVICE", "MMLTK_TRAINING_TEST_DEVICE_IDS", "MMLTK_GDR_TEST_BACKEND",
    "TSAN_OPTIONS", "MMLTK_GUI_TRACE_FILE", "MMLTK_GUI_PIXEL_TRACE"})
   if (auto value = env(name); !value.empty()) {
    if (std::string_view(name) == "MMLTK_GUI_TRACE_FILE") value = host_path(value);
    append(args, {"--env", std::string(name) + "=" + value});
   }
 }
 int test_program(const std::string& executable_path, const Args& arguments) {
  owned_dirs({cache + "/tests/rfdetr"});
  common_groups();
  Args args{
   "run", "--rm", "--init", "-i", "--name", test_run_container, "--security-opt", "seccomp=unconfined", "--gpus", "all", "--ipc", "host", "--network", "host", "--user", user(), "--env",
   "HOME=" + container_cache + "/container-home", "--env", "NVIDIA_DRIVER_CAPABILITIES=all", "--env", "MMLTK_REPO_ROOT=/workspace", "--env",
   "MMLTK_RFDETR_TEST_CACHE_DIR=" + container_cache + "/tests/rfdetr", "--volume", repo + ":/workspace:rw", "--volume", "/:/host:rw", "--workdir", "/workspace"
  };
  execution_policy(args);
  labels(args, mode);
  const bool tty = isatty(0) && isatty(1);
  if (tty) args.push_back("-t");
  logging_environment(args, "test", arguments);
  forwarded_test_environment(args);
  explicit_test_environment(args);
  append(args, groups);
  if (debugger) {
   append(args, {"--entrypoint", "/usr/bin/gdb", build_image});
   if (!tty) args.push_back("--batch");
   for (const auto& command : debugger_commands) append(args, {"-ex", command});
   if (debugger_commands.empty() && !tty) append(args, {"-ex", "run", "-ex", "bt"});
   append(args, {"--args", executable_path});
  } else
   append(args, {"--entrypoint", executable_path, build_image});
  append(args, arguments);
  return owned_docker(test_run_container, args).status;
 }
 Result workspace_program(const std::string& executable_path, const Args& arguments, bool capture_output = false) {
  require(docker_ok({"image", "inspect", image_name}), "workspace-wayland requires the packaged runtime image; run ./mmltk --build first");
  owned_dirs({cache + "/container-home", cache + "/tests/rfdetr", repo + "/build/validation"});
  prepare_mounts(true);
  common_groups();
  Args args{
   "run", "--rm", "--init", "-i", "--name", test_run_container, "--privileged", "--gpus", "all", "--ipc", "host", "--network", "host", "--user", user(), "--tmpfs", tmpfs(), "--env",
   "HOME=" + container_cache + "/container-home", "--env", "NVIDIA_DRIVER_CAPABILITIES=all", "--env", "MMLTK_REPO_ROOT=/workspace", "--env",
   "MMLTK_RFDETR_TEST_CACHE_DIR=" + container_cache + "/tests/rfdetr", "--env", "MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION=1", "--volume", repo + ":/workspace:rw", "--volume", "/:/host:rw", "--workdir",
   "/workspace"
  };
  execution_policy(args);
  native_environment(args);
  labels(args, mode);
  if (isatty(0) && isatty(1) && !capture_output) args.push_back("-t");
  append_gui_environment(args);
  logging_environment(args, "wayland", arguments);
  explicit_test_environment(args);
  append(args, groups);
  for (const auto& mount : mounts) append(args, {"--volume", mount});
  append(args, commands::workspace_test_command(headless, executable_path, arguments, image_name, validation_image));
  headless_active = headless;
  const auto result = owned_docker(test_run_container, args, {.capture = capture_output});
  headless_active = false;
  return result;
 }
 int headless_tool_tests(const Args& arguments = {}) {
  const auto id = existing_diagnostic_image(build_image, "docker daemon is unavailable", "headless compositor tooling requires existing image '" + build_image + "'"),
             name = container_name + "-headless-tool";
  Args args{
   "run", "--rm", "--init", "--name", name, "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--ulimit", "core=1:1", "--network", "none", "--user", user(), "--env",
   "PYTHONDONTWRITEBYTECODE=1", "--volume", repo + ":/workspace:ro", "--tmpfs", "/tmp:rw,nosuid,nodev,size=32m", "--workdir", "/workspace", "--entrypoint", "/workspace/build/tools/mmltk"
  };
  labels(args, mode);
  args.push_back(id);
  append(args, tail(tool_arguments(Command::test_headless_compositor, arguments)));
  return owned_docker(name, args).status;
 }
 int browser_tests(Args arguments) {
  std::string js_pattern, rust_package;
  while (!arguments.empty() && (arguments[0] == "--js-test-name-pattern" || arguments[0] == "--rust-package")) {
   require(arguments.size() >= 2 && !arguments[1].empty(), arguments[0] + " requires a value");
   (arguments[0] == "--js-test-name-pattern" ? js_pattern : rust_package) = arguments[1];
   arguments = tail(arguments, 2);
  }
  require(js_pattern.empty() || rust_package.empty(), "select JavaScript cases or a Rust package in one browser-app invocation");
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  Args escaped;
  for (const auto& argument : arguments) escaped.push_back(replace(argument, ";", "\\;"));
  const std::string configuration =
   quoted({"-DMMLTK_ICED_TEST_ARGUMENTS=" + joined(escaped, ";"), "-DMMLTK_ICED_JS_TEST_PATTERN=" + replace(js_pattern, ";", "\\;"), "-DMMLTK_ICED_TEST_PACKAGE=" + rust_package});
  quiet_log("running iced browser application tests");
  const std::string command = R"MMLTK(set -euo pipefail
browser_test_icds=""
for browser_test_icd in /etc/vulkan/icd.d/*nvidia*.json /usr/share/vulkan/icd.d/*nvidia*.json; do
 if [[ -r "${browser_test_icd}" ]]; then browser_test_icds+="${browser_test_icds:+:}${browser_test_icd}"; fi
done
if [[ -z "${browser_test_icds}" ]]; then
 printf "%s\n" "browser-app tests failed: container NVIDIA Vulkan ICD is unavailable" >&2
 exit 1
fi
export VK_DRIVER_FILES="${browser_test_icds}"
export WGPU_BACKEND=vulkan
)MMLTK" + tool_shell(Command::configure_cmake_graph) +
                              R"MMLTK( gui "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui" )MMLTK" + configuration + R"MMLTK(
cmake --build "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui" --parallel "${MMLTK_JOBS}" --target mmltk_frontend_iced_tests
)MMLTK" + tool_shell(Command::check_toolchain_invariants) +
                              R"MMLTK( --repo-root /workspace --build-dir "${MMLTK_CONTAINER_CACHE_ROOT}/cmake/gui"
)MMLTK";
  const int status = owned_toolchain(test_run_container, command, "graphics,utility");
  release_mutation();
  return status;
 }
 static const Args& all_targets() {
  static const Args targets{
   "mmltk_acceptance",
   "mmltk_backend_data_tests",
   "mmltk_backend_imaging_resample_tests",
   "mmltk_backend_imaging_raster_tests",
   "mmltk_backend_imaging_annotation_tests",
   "mmltk_backend_imaging_explore_tests",
   "mmltk_backend_imaging_upscale_tests",
   "mmltk_backend_media_live_tests",
   "mmltk_backend_ml_layers_tests",
   "mmltk_backend_ml_cuda_tests",
   "mmltk_backend_models_catalog_tests",
   "mmltk_backend_models_rfdetr_contract_tests",
   "mmltk_backend_models_rfdetr_core_tests",
   "mmltk_backend_models_rfdetr_augmentation_tests",
   "mmltk_backend_models_rfdetr_export_tests",
   "mmltk_backend_media_video_tests",
   "mmltk_backend_models_rfdetr_inference_tests",
   "mmltk_backend_models_rfdetr_training_tests",
   "mmltk_common_system_tests",
   "mmltk_common_concurrency_tests",
   "mmltk_controller_annotation_tests",
   "mmltk_controller_browser_tests",
   "mmltk_controller_visual_systems_tests",
   "mmltk_controller_explore_tests",
   "mmltk_controller_upscale_tests",
   "mmltk_controller_live_tests",
   "mmltk_controller_services_tests",
   "mmltk_test_support_tests",
   "mmltk_controller_shell_tests",
   "mmltk_controller_data_compute_systems_tests",
   "mmltk_entrypoints_cli_tests",
   "mmltk_entrypoints_desktop_tests",
   "mmltk_controller_firefox_process_tests",
   "mmltk_entrypoints_tools_tests",
   "mmltk_frameworks_gpu_tests",
   "mmltk_frameworks_process_tests",
   "mmltk_frameworks_serialization_tests",
   "mmltk_frameworks_transport_tests",
   "mmltk_workspace_wayland_integration",
  };
  return targets;
 }
 static const std::map<std::string, Args>& native_suites() {
  static const std::map<std::string, Args> suites{
   {"application-systems",
    {
     "mmltk_controller_annotation_tests",
     "mmltk_controller_browser_tests",
     "mmltk_controller_services_tests",
     "mmltk_test_support_tests",
     "mmltk_controller_shell_tests",
     "mmltk_controller_data_compute_systems_tests",
     "mmltk_controller_visual_systems_tests",
     "mmltk_controller_explore_tests",
     "mmltk_controller_upscale_tests",
     "mmltk_controller_live_tests",
     "mmltk_frameworks_gpu_tests",
     "mmltk_backend_media_live_tests",
     "mmltk_backend_media_video_tests",
    }},
   {"application-contracts",
    {
     "mmltk_controller_annotation_tests",
     "mmltk_controller_browser_tests",
     "mmltk_controller_services_tests",
     "mmltk_test_support_tests",
     "mmltk_controller_visual_systems_tests",
     "mmltk_controller_explore_tests",
     "mmltk_controller_upscale_tests",
     "mmltk_controller_live_tests",
     "mmltk_frameworks_serialization_tests",
    }},
   {"transport",
    {
     "mmltk_frameworks_transport_tests",
    }},
   {"browser-runtime",
    {
     "mmltk_entrypoints_desktop_tests",
     "mmltk_controller_firefox_process_tests",
    }},
   {"workspace-wayland",
    {
     "mmltk_workspace_wayland_integration",
    }},
   {"cuda-vulkan",
    {
     "mmltk_cuda_vulkan_interop",
    }},
   {"core",
    {
     "mmltk_controller_visual_systems_tests",
     "mmltk_acceptance",
     "mmltk_backend_data_tests",
     "mmltk_backend_imaging_resample_tests",
     "mmltk_backend_models_catalog_tests",
     "mmltk_common_system_tests",
     "mmltk_common_concurrency_tests",
     "mmltk_entrypoints_cli_tests",
     "mmltk_entrypoints_tools_tests",
    }},
   {"rfdetr",
    {
     "mmltk_backend_ml_layers_tests",
     "mmltk_backend_ml_cuda_tests",
     "mmltk_backend_imaging_raster_tests",
     "mmltk_backend_models_rfdetr_contract_tests",
     "mmltk_backend_models_rfdetr_core_tests",
     "mmltk_backend_models_rfdetr_augmentation_tests",
     "mmltk_backend_models_rfdetr_export_tests",
     "mmltk_backend_media_video_tests",
     "mmltk_backend_models_rfdetr_inference_tests",
     "mmltk_backend_models_rfdetr_training_tests",
    }},
   {"rfdetr-profile",
    {
     "mmltk_rfdetr_train_profile_runner",
    }},
   {"data-profile", {
                     "mmltk_backend_data_profile_runner",
                    }},
  };
  return suites;
 }
 int run_tests(const std::string& selected, Args arguments) {
  require(!headless || selected == "workspace-wayland", "--headless-compositor is only valid with --test workspace-wayland");
  if (selected == "list" || selected == "help" || selected == "-h" || selected == "--help") {
   require(arguments.empty(), "--test " + selected + " does not accept additional arguments");
   std::cout << commands::test_help;
   return 0;
  }
  if (selected == "gui" || selected == "tsan") die("--test " + selected + " is unavailable in the native Catch2 test graph");
  if (selected == "headless-compositor" || selected == "headless-compositor-tool" || selected == "cleanup-tool" || selected == "log-query-tool") {
   require(!test_config_explicit, "--test " + selected + " does not accept --config");
   require(test_executable.empty(), "--test " + selected + " does not accept --executable");
   require(!debugger, "--test " + selected + " does not accept --gdb or --gdb-command");
   require(test_environment.empty(), "--test " + selected + " does not accept --env");
   begin_test(selected);
   if (selected == "headless-compositor-tool") { return headless_tool_tests(arguments); }
   if (selected == "cleanup-tool") return cleanup_tool_tests(arguments);
   if (selected == "log-query-tool") {
    quiet_log("running log query tooling fixture tests");
    return log_tool(true, arguments);
   }
   acquire_mutation();
   ensure_docker();
   cache_layout();
   ensure_validation_image();
   headless = true;
   if (arguments.empty()) arguments.push_back("/usr/bin/true");
   const int status = workspace_program(arguments[0], tail(arguments)).status;
   release_mutation();
   return status;
  }
  if (selected == "browser-app") {
   require(test_config == "release", "--test browser-app owns its GUI graph and does not accept --config");
   require(test_executable.empty(), "--test browser-app does not accept --executable");
   require(!debugger, "--test browser-app does not accept --gdb");
   require(test_environment.empty(), "--test browser-app does not yet consume native test environments");
   begin_test(selected);
   return browser_tests(arguments);
  }
  if (selected == "rfdetr-profile") {
   require(!test_config_explicit || test_config == "dev", "rfdetr-profile requires the instrumented dev configuration");
   test_config = "dev";
  }
  Args targets, executables;
  if (selected == "build" || selected == "all") {
   require(selected != "build" || arguments.empty(), "--test build does not accept Catch2 arguments");
   targets = all_targets();
   if (selected == "all")
    for (const auto& target : targets)
     if (target != "mmltk_workspace_wayland_integration") executables.push_back(target);
  } else if (const auto it = native_suites().find(selected); it != native_suites().end()) {
   targets = it->second;
   executables = targets;
  } else {
   std::cerr << commands::test_help;
   die("unknown test suite '" + selected + "'");
  }
  if (!test_executable.empty()) {
   require(std::find(targets.begin(), targets.end(), test_executable) != targets.end(), "test executable '" + test_executable + "' is not owned by suite '" + selected + "'");
   targets = {test_executable};
   if (selected != "build") {
    require(std::find(executables.begin(), executables.end(), test_executable) != executables.end(), "target '" + test_executable + "' is not an executable in suite '" + selected + "'");
    executables = {test_executable};
   }
  }
  const bool workspace = selected == "workspace-wayland", cuda_vulkan = selected == "cuda-vulkan";
  if (workspace) {
   require(test_config == "release", "workspace-wayland requires the packaged release graph");
   require(!debugger, "workspace-wayland does not run through the build-image debugger");
  }
  if (cuda_vulkan) {
   require(test_config == "release", "cuda-vulkan requires the release graph");
   require(!debugger, "cuda-vulkan runs in the existing validation image without a debugger");
   require(test_environment.empty(), "cuda-vulkan accepts positional program arguments, not --env");
  }
  require(!debugger || executables.size() == 1, "--gdb requires a suite with one executable or an explicit --executable");
  begin_test(selected);
  acquire_mutation();
  ensure_docker();
  cache_layout();
  ensure_build_image();
  const std::string enabled = test_config == "release" ? "ON" : "OFF";
  if (test_config == "release") firefox_build_input();
  std::string inventory;
  if (selected == "all" || selected == "build")
   inventory = tool_shell(Command::audit_test_inventory) + " --compile-database \"${MMLTK_CONTAINER_CACHE_ROOT}/cmake/" + test_config +
               "/compile_commands.json\" --repo-root /workspace --expected-targets " + shell_quote(joined(all_targets()));
  quiet_log("building cached " + selected + " test graph with " + std::to_string(jobs) + " workers");
  const std::string directory = "\"${MMLTK_CONTAINER_CACHE_ROOT}/cmake/" + test_config + "\"";
  const std::string command = "set -euo pipefail\n" + tool_shell(Command::configure_cmake_graph) + " " + test_config + " " + directory + " -DBUILD_MMLTK_BROWSER_HOST=" + enabled +
                              " -DBUILD_MMLTK_FIREFOX_RUNTIME=" + enabled + " -DMMLTK_BUILD_JOBS=\"${MMLTK_JOBS}\" -DMMLTK_FIREFOX_CACHE_ROOT=\"${MMLTK_FIREFOX_CACHE_ROOT}\"\n" + inventory +
                              "\ncmake --build " + directory + " --parallel \"${MMLTK_JOBS}\" --target " + quoted(targets) + "\n" + tool_shell(Command::check_toolchain_invariants) +
                              " --repo-root /workspace --build-dir " + directory;
  must_toolchain(test_build_container, command);
  if (cuda_vulkan) {
   ensure_validation_image();
   const int status = gpu_executable(cmake + "/" + test_config + "/" + executables[0], arguments);
   release_mutation();
   return status;
  }
  if (workspace) {
   if (headless) ensure_validation_image();
   quiet_log("enumerating " + executables[0] + " hardware cases");
   const auto executable_path = container_cache + "/cmake/" + test_config + "/" + executables[0];
   const auto listing = workspace_program(executable_path, {"--list-tests"}, true);
   if (listing.status) {
    release_mutation();
    die("workspace-wayland test enumeration failed");
   }
   std::cout << mmltk::tools::text::trim_newlines(listing.out) << '\n';
   for (const auto* name :
    {"workspace_wayland_retained", "workspace_wayland_workflows", "workspace_wayland_dpi", "workspace_wayland_terminal", "workspace_wayland_probe_recovery", "workspace_wayland_gdr",
     "workspace_wayland_quiet"})
    if (listing.out.find(name) == std::string::npos) {
     release_mutation();
     die("workspace-wayland executable is missing expected hardware case '" + std::string(name) + "'");
    }
   quiet_log("running " + executables[0]);
   const int status = workspace_program(executable_path, arguments).status;
   release_mutation();
   return status;
  }
  int first_failure = 0;
  for (const auto& executable_name : executables) {
   quiet_log("running " + executable_name);
   const int status = test_program(container_cache + "/cmake/" + test_config + "/" + executable_name, arguments);
   if (!status) continue;
   if (!first_failure) first_failure = status;
   if (selected != "all") {
    release_mutation();
    return status;
   }
  }
  release_mutation();
  if (selected == "all" && test_executable.empty() && arguments.empty()) {
   const auto browser_status = browser_tests({});
   if (!first_failure) first_failure = browser_status;
   int cleanup_status = 0;
   try {
    cleanup_status = cleanup_tool_tests({});
   } catch (const Exit& error) {
    cleanup_status = error.status;
    release_mutation();
   }
   if (!first_failure) first_failure = cleanup_status;
   quiet_log("running log query tooling fixture tests");
   int log_status = 0;
   try {
    log_status = log_tool(true, {});
   } catch (const Exit& error) { log_status = error.status; }
   if (!first_failure) first_failure = log_status;
   int compositor_status = 0;
   try {
    compositor_status = headless_tool_tests();
   } catch (const Exit& error) { compositor_status = error.status; }
   if (!first_failure) first_failure = compositor_status;
  }
  return first_failure;
 }
 void parse_tests(Args arguments) {
  test_args.clear();
  for (std::size_t i = 0; i < arguments.size(); ++i) {
   const auto& argument = arguments[i];
   if (argument == "--") {
    append(test_args, tail(arguments, i + 1));
    break;
   }
   if (argument == "--gdb") {
    debugger = true;
    continue;
   }
   if (argument == "--headless-compositor") {
    headless = true;
    continue;
   }
   if (argument == "--config" || argument == "--executable" || argument == "--env" || argument == "--gdb-command") {
    const std::string message = argument == "--config"       ? "--config requires release or dev"
                                : argument == "--executable" ? "--executable requires a target name"
                                 : argument == "--env"       ? "--env requires NAME or NAME=VALUE"
                                                             : "--gdb-command requires one GDB command";
    require(i + 1 < arguments.size(), message);
    const auto value = arguments[++i];
    if (argument == "--config") {
     test_config = value;
     test_config_explicit = true;
    } else if (argument == "--executable")
     test_executable = value;
    else if (argument == "--gdb-command") {
     debugger = true;
     debugger_commands.push_back(value);
    } else {
     const auto name = value.substr(0, value.find('='));
     require(matches(name, "[A-Za-z_][A-Za-z0-9_]*"), "invalid --env name '" + name + "'");
     test_environment.push_back(value);
    }
   } else
    test_args.push_back(argument);
  }
  require(test_config == "release" || test_config == "dev", "--config must be release or dev");
  if (suite == "tsan") jobs = std::min(jobs, 8L);
 }
 void parse() {
  const std::map<std::string, bool*> flags{
   {"--gui", &gui}, {"--build", &build}, {"--build-gui", &build_gui}, {"--generate-protocol", &protocol}, {"--generate-application-bindings", &bindings}, {"--export-shiftlut", &export_shiftlut},
   {"--update-gui-lock", &update_gui}, {"--update-firefox-lock", &update_firefox}, {"--tidy", &tidy}, {"--build-tools", &build_tools}, {"--tidy-tools", &tidy_tools}, {"--third_party", &third_party}
  };
  for (const auto& argument : original) {
   if (argument == "--prepare-gui-container") {
    gui = true;
    prepare_gui = true;
    continue;
   }
   if (const auto it = flags.find(argument); it != flags.end())
    *it->second = true;
   else
    passthrough.push_back(argument);
  }
  require(!third_party || tidy, "--third_party is only valid with --tidy");
  if (!passthrough.empty()) {
   const auto arg = passthrough[0];
   if (arg == "--test" || starts(arg, "--test=")) {
    test = true;
    if (arg == "--test") {
     require(passthrough.size() >= 2, "--test requires a suite name; use './mmltk --test list'");
     suite = passthrough[1];
     test_args = tail(passthrough, 2);
    } else {
     suite = arg.substr(7);
     require(!suite.empty(), "--test requires a suite name; use './mmltk --test list'");
     test_args = tail(passthrough);
    }
   } else if (arg == "--diagnose-gpu-environment") {
    diagnose_gpu = true;
    require(passthrough.size() <= 2, "--diagnose-gpu-environment accepts at most one existing image selection");
    gpu_image = passthrough.size() > 1 && !passthrough[1].empty() ? passthrough[1] : "runtime";
    passthrough.clear();
   } else if (arg == "--diagnose-nvidia-payload") {
    diagnose_nvidia = true;
    require(passthrough.size() >= 3, "--diagnose-nvidia-payload requires donor or development, followed by public header filenames");
    nvidia_args = tail(passthrough);
    passthrough.clear();
   } else if (arg == "--diagnose-io") {
    diagnose_io = true;
    require(passthrough.size() == 2, "--diagnose-io requires exactly one compiled-file path");
    io_path = passthrough[1];
    passthrough.clear();
   } else if (arg == "--raw-cpd" || arg == "--format-declarations" || arg == "--audit-includes") {
    declaration = true;
    declaration_operation = arg == "--raw-cpd" ? "raw" : arg == "--format-declarations" ? "format" : "includes";
    declaration_args = tail(passthrough);
    passthrough.clear();
   } else if (arg == "--cleanup-report" || starts(arg, "--cleanup-report=")) {
    cleanup_report = true;
    if (arg == "--cleanup-report") {
     require(passthrough.size() == 2, "--cleanup-report requires exactly one of cpp, frontend, tools, or all");
     cleanup_profile = passthrough[1];
    } else {
     require(passthrough.size() == 1, "--cleanup-report does not accept additional arguments");
     cleanup_profile = arg.substr(17);
     require(!cleanup_profile.empty(), "--cleanup-report requires cpp, frontend, tools, or all");
    }
    passthrough.clear();
   }
  }
  for (const auto& argument : passthrough) {
   require(argument != "--diagnose-io", "--diagnose-io must be the standalone operation before its compiled-file path");
   require(argument != "--diagnose-nvidia-payload", "--diagnose-nvidia-payload must be the standalone operation before its source and public header filenames");
   require(argument != "--cleanup-report" && !starts(argument, "--cleanup-report="), "--cleanup-report must be the standalone operation before its profile");
   require(argument != "--raw-cpd" && argument != "--format-declarations" && argument != "--audit-includes", "--raw-cpd, --format-declarations and --audit-includes must be standalone operations");
   for (const auto* standalone :
    {"--logs", "--diagnose-gpu-environment", "--diagnose-gpu-program", "--diagnose-native-symbols", "--diagnose-rfdetr-upstream", "--diagnose-processes", "--diagnose-benchmark-image",
     "--diagnose-native-link"})
    if (argument == standalone)
     die(argument + (argument == "--diagnose-rfdetr-upstream" ? " must be first and standalone; use './mmltk " : " must be the standalone operation; use './mmltk ") + argument + " --help'");
  }
  if (test) parse_tests(test_args);
  const int count = build + build_gui + protocol + bindings + export_shiftlut + update_gui + update_firefox + tidy + tidy_tools + build_tools + test + cleanup_report + declaration + diagnose_io +
                    diagnose_nvidia + diagnose_gpu;
  require(
   count <= 1 && !(gui && (build || build_tools || tidy || tidy_tools || test || cleanup_report || declaration || diagnose_io || diagnose_nvidia || diagnose_gpu || export_shiftlut || update_firefox)),
   "--build, --build-tools, --tidy-tools, --build-gui, --generate-protocol, --generate-application-bindings, --export-shiftlut, --update-gui-lock, --update-firefox-lock, --test, --tidy, "
   "--cleanup-report, --diagnose-io, --diagnose-nvidia-payload, and --diagnose-gpu-environment are standalone");
 }

public:
 Wrapper(int argc, char** argv, std::optional<ContainerUser> bootstrap_user = {}) {
  original.assign(argv + 1, argv + argc);
  repo = support::repository_root(argv[0]).string();
  self = support::executable_path().string();
  host_workdir = env("PWD", fs::current_path().string());
  tools_image = env("MMLTK_TOOLS_IMAGE", "mmltk-tools:latest");
  image_name = env("MMLTK_IMAGE", "mmltk");
  validation_image = image_name + "-wayland-validation";
  build_image = env("MMLTK_BUILD_IMAGE", "mmltk-build:latest");
  toolchain_image = env("MMLTK_TOOLCHAIN_BASE_IMAGE", "mmltk-toolchain-base:latest");
  gcc_image = env("MMLTK_GCC_IMAGE", "mmltk-gcc16:latest");
  payload_image = env("MMLTK_NVIDIA_PAYLOAD_IMAGE", "mmltk-nvidia-payload:latest");
  runtime_base = env("MMLTK_RUNTIME_BASE_IMAGE", "mmltk-runtime-base:latest");
  ubuntu_image = env("MMLTK_UBUNTU_IMAGE", "mmltk-ubuntu:latest");
  ort_image = env("MMLTK_ONNXRUNTIME_IMAGE", "mmltk-onnxruntime:latest");
  dependencies_image = env("MMLTK_DEPENDENCIES_IMAGE", "mmltk-dependencies:latest");
  analysis_image = env("MMLTK_ANALYSIS_IMAGE", image_name + "-analysis");
  auto requested_cache = env("MMLTK_CACHE_ROOT", repo + "/.cache");
  if (!starts(requested_cache, "/")) requested_cache = repo + "/" + requested_cache;
  cache = normalized(requested_cache);
  repository_cache = repo + "/.cache";
  buildkit = cache + "/buildkit";
  cmake = cache + "/cmake";
  cargo = cache + "/cargo";
  ccache = cache + "/ccache";
  firefox = cache + "/firefox";
  firefox_input = firefox + "/build-input.sha256";
  fingerprints = cache + "/image-fingerprints";
  locks = cache + "/locks";
  auto requested_stage = env("MMLTK_RELEASE_STAGE_ROOT", repo + "/build/release");
  if (!starts(requested_stage, "/")) requested_stage = repo + "/" + requested_stage;
  release_stage = normalized(requested_stage);
  quiet_gui = std::find(original.begin(), original.end(), "--gui") != original.end();
  const auto log_level = env("MMLTK_LOG_LEVEL");
  if (!env("MMLTK_GUI_TRACE_FILE").empty() || env("MMLTK_ANNOTATION_DIAGNOSTICS") == "1" || env("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION") == "1" ||
      (lower(log_level) != "off" && !(log_level + env("MMLTK_LOG_FILE") + env("MMLTK_LOG_DIR")).empty()))
   quiet_gui = false;
  require(access("/sys/devices/system/cpu/online", R_OK) == 0, "Linux online CPU list is unavailable");
  cpu_list = mmltk::tools::text::trim_newlines(read_text("/sys/devices/system/cpu/online"));
  require(matches(cpu_list, "[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*"), "invalid Linux online CPU list: " + cpu_list);
  const auto job_count = get({"getconf", "_NPROCESSORS_ONLN"});
  require(matches(job_count, "[1-9][0-9]*"), "invalid online CPU count: " + job_count);
  jobs = std::stol(job_count);
  require(within(cache, repository_cache), "MMLTK_CACHE_ROOT must stay under " + repository_cache);
  require(within(release_stage, repo + "/build", false), "MMLTK_RELEASE_STAGE_ROOT must stay under " + repo + "/build");
  container_cache = "/workspace" + cache.substr(repo.size());
  container_stage = "/workspace" + release_stage.substr(repo.size());
  sccache_basedirs = container_cache + "/firefox/obj-minimal-opt:/workspace/third_party/firefox:" + container_cache + "/firefox/mozbuild/sysroot-x86_64-linux-gnu";
  trace_file = env("MMLTK_BUILD_TRACE_FILE");
  ccache_stats = env("MMLTK_CCACHE_STATS_FILE");
  sccache_stats = env("MMLTK_FIREFOX_SCCACHE_STATS_FILE");
  const auto selected = bootstrap_user ? std::move(*bootstrap_user) : resolve_container_user([&](const Args& args) { return get(args); });
  uid = selected.uid;
  gid = selected.gid;
  if (getuid() == 0) {
   if (selected.target.empty())
    log("warning: running as root, no real user found (set MMLTK_USER to override)");
   else
    quiet_log("running as root, resolved container user to " + selected.target + " (" + user() + ")");
  }
  const auto hash = sha(repo).substr(0, 12);
  base_name = env("MMLTK_CONTAINER_NAME", "mmltk-" + hash);
  container_name = base_name + "-" + std::to_string(getpid());
  release_container = base_name + "-build-release";
  package_container = base_name + "-build-package";
  gui_container = base_name + "-build-gui";
  test_build_container = base_name + "-test-build";
  test_run_container = base_name + "-test-run";
  tidy_container = base_name + "-tidy";
  cleanup_container = base_name + "-cleanup-report";
  builder = env("MMLTK_BUILDX_BUILDER", base_name + "-buildx");
  wrapper_start = start_ticks(wrapper_pid);
 }
 int execute() {
  const auto initial = first(original);
  const auto arguments = tail(original);
  if (initial == "--logs") {
   mode = "logs";
   return log_tool(false, arguments);
  }
  if (initial == "--diagnose-gpu-program") {
   mode = "diagnose-gpu-program";
   return gpu_program(arguments);
  }
  if (initial == "--diagnose-native-symbols") {
   mode = "diagnose-native-symbols";
   return native_diagnostic(false, arguments);
  }
  if (initial == "--diagnose-processes") {
   mode = "diagnose-processes";
   return process_diagnostic(arguments);
  }
  if (initial == "--diagnose-benchmark-image") {
   mode = "diagnose-benchmark-image";
   return benchmark_image(arguments);
  }
  if (initial == "--diagnose-native-link") {
   mode = "diagnose-native-link";
   return native_diagnostic(true, arguments);
  }
  if (initial == "--diagnose-rfdetr-upstream") {
   mode = "diagnose-rfdetr-upstream";
   return oracle(arguments);
  }
  parse();
  if (export_shiftlut) {
   require(passthrough.size() == 3, "--export-shiftlut requires --source PATH and --export-only or --verify-only");
   require(passthrough[0] == "--source", "ShiftLUT requires --source PATH");
   shiftlut_source = existing(passthrough[1]);
   require(fs::is_directory(shiftlut_source + "/LUT_test/LUTs/ShiftLUT_sr_s7_int"), "missing ShiftLUT s7 tables");
   shiftlut_operation = passthrough[2];
   require(shiftlut_operation == "--export-only" || shiftlut_operation == "--verify-only", "ShiftLUT requires --export-only or --verify-only");
   mode = "export-shiftlut";
   acquire_mutation();
   ensure_docker();
   cache_layout();
   ensure_build_image();
   quiet_log("ShiftLUT " + shiftlut_operation);
   must_toolchain(release_container + "-shiftlut-export", commands::shiftlut);
   release_mutation();
   return 0;
  }
  if (diagnose_gpu) {
   mode = "diagnose-gpu-environment";
   return gpu_environment(gpu_image);
  }
  if (diagnose_nvidia) {
   mode = "diagnose-nvidia-payload";
   return nvidia_diagnostic(nvidia_args);
  }
  if (diagnose_io) {
   mode = "diagnose-io";
   return io_diagnostic(io_path);
  }
  if (test) {
   mode = "test";
   if (suite != "list" && suite != "help" && suite != "-h" && suite != "--help") {
    prepare_logging(env("MMLTK_TEST_LOG_FILE"));
    log("test suite: " + suite);
   }
   return run_tests(suite, test_args);
  }
  if (declaration) {
   mode = "declaration-" + declaration_operation;
   return declaration_tool(declaration_operation, declaration_args);
  }
  if (cleanup_report) {
   mode = "cleanup-report";
   cleanup_reports(cleanup_profile);
   return 0;
  }
  rewrite_args();
  if (build_tools) {
   if (passthrough == Args{"--help"} || passthrough == Args{"-h"}) {
    std::cout << "Usage: ./mmltk --build-tools\nBuild the standalone GCC 16.2/C++26 tools graph and publish ./mmltk (optimized standalone executable).\nUse --tidy-tools [--file PATH...] [--start-at "
                 "PATH] for native tools "
                 "analysis.\n";
    return 0;
   }
   require(passthrough.empty(), "--build-tools does not accept application arguments");
   mode = "build-tools";
   prepare_logging(env("MMLTK_BUILD_LOG_FILE"));
   acquire_mutation();
   prepare_tools(true);
   release_mutation();
   return 0;
  }
  if (tidy || tidy_tools) {
   mode = "tidy";
   run_tidy();
   return 0;
  }
  if (build) {
   require(passthrough.empty(), "--build does not accept application arguments");
   mode = "build";
   prepare_logging(env("MMLTK_BUILD_LOG_FILE"));
   run_build();
   return 0;
  }
  if (protocol || bindings) {
   const auto flag = bindings ? "--generate-application-bindings" : "--generate-protocol";
   require(passthrough.empty(), std::string(flag) + " does not accept application arguments");
   mode = bindings ? "generate-application-bindings" : "generate-protocol";
   generation(bindings);
   return 0;
  }
  if (update_gui || update_firefox) {
   const std::string owner = update_firefox ? "firefox" : "gui";
   require(passthrough.empty(), "--update-" + owner + "-lock does not accept application arguments");
   mode = "update-" + owner + "-lock";
   lock_update(update_firefox);
   return 0;
  }
  if (build_gui) {
   require(gui || passthrough.empty(), "--build-gui without --gui does not accept application arguments");
   mode = "build-gui";
   gui_build();
   if (!gui) return 0;
  }
  mode = gui ? "gui" : "raw";
  acquire_runtime();
  prepare_invocation();
  if (prepare_gui) {
   release_runtime();
   return 0;
  }
  auto args = execution_args;
  append(args, container_command);
  const auto result = docker(args);
  release_runtime();
  return result.status;
 }
 int cancel(int signal_number) {
  cancelling = 1;
  try {
   if (headless_active) run(timeout({"docker", "stop", "--time", "22", test_run_container}, 25), {.quiet = true});
   if (gui && cleanup_armed)
    run(timeout({"docker", "exec", "-u", "0", container_name, "/bin/sh", "-lc", std::string(commands::shutdown), "mmltk-gui-shutdown", signal_number == SIGINT ? "INT" : "TERM", "100"}, 15),
     {.quiet = true});
   if (active_docker > 0) {
    kill(active_docker, signal_number);
    if (!wait_pid(active_docker, 2000)) kill(active_docker, SIGTERM);
    if (!wait_pid(active_docker, 2000)) {
     kill(active_docker, SIGKILL);
     wait_pid(active_docker, 1000);
    }
   }
   const auto remaining = processes;
   for (const auto& child : remaining) stop_process(child.pid, child.start, getpid());
   for (const auto& child : remaining) { reap(child.pid); }
   processes.clear();
   active_docker = 0;
   terminate_builds();
  } catch (const std::exception& error) { log("shutdown failed: " + std::string(error.what())); }
  return 128 + signal_number;
 }
 int finish(int status) {
  if (done) return status;
  done = true;
  cancelling = 1;
  auto cleanup_failure = [&] {
   if (!status) status = 1;
  };
  try {
   terminate_builds();
   if (cleanup_armed) {
    cleanup_armed = false;
    if (run(timeout({"docker", "rm", "-f", container_name}, 15), {.quiet = true}).status) run(timeout({"docker", "rm", "-f", container_name}, 5), {.quiet = true});
    const auto result = run(timeout({"docker", "container", "ls", "-a", "--filter", "name=^/" + container_name + "$", "--format", "{{.Names}}"}, 5), {.capture = true, .stderr_quiet = true});
    if (result.status || !trim(result.out).empty()) {
     log("container cleanup did not complete within its bounded deadline");
     cleanup_failure();
    }
   }
  } catch (const std::exception& error) {
   log("container cleanup failed: " + std::string(error.what()));
   cleanup_failure();
  } catch (const Exit&) { cleanup_failure(); }
  if (!log_pids.empty()) {
   std::cout.flush();
   std::cerr.flush();
   dup2(saved_stdout.value, STDOUT_FILENO);
   dup2(saved_stderr.value, STDERR_FILENO);
   saved_stdout.reset();
   saved_stderr.reset();
   for (const auto pid : log_pids) {
    int result = 0;
    pid_t waited;
    do { waited = waitpid(pid, &result, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || exit_status(result)) {
     log(mode + " log capture failed: " + operation_log);
     cleanup_failure();
    }
   }
   const auto terminal = mode + " exited with status " + std::to_string(status);
   log(terminal);
   try {
    Fd fd(open(operation_log.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC));
    write_fd(fd.value, "mmltk: " + terminal + "\n");
   } catch (...) {
    log("could not record terminal status in " + operation_log);
    cleanup_failure();
   }
  }
  for (const auto& path : {test_registration, runtime_registration})
   if (!path.empty()) {
    std::error_code error;
    fs::remove(path, error);
    if (error) {
     log("cannot remove owner registration '" + path + "': " + error.message());
     cleanup_failure();
    }
   }
  tools_stage_lock.reset();
  release_runtime();
  release_mutation();
  return status;
 }
};
}  // namespace
int main(int argc, char** argv) {
 try {
  auto bootstrap_user = consume_bootstrap_handoff();
  if (argc > 1 && std::string_view(argv[1]) == "--tool") return mmltk::tools::dispatch::entry(argc - 2, argv + 2);
  install_signals();
  Wrapper wrapper(argc, argv, std::move(bootstrap_user));
  int status = 0;
  try {
   status = wrapper.execute();
   interrupted();
  } catch (const Exit& error) { status = error.status; } catch (const Signal& signal) {
   status = wrapper.cancel(signal.number);
  } catch (const std::exception& error) {
   log(error.what());
   status = 1;
  }
  if (pending_signal && !cancelling) status = wrapper.cancel(pending_signal);
  return wrapper.finish(status);
 } catch (const Exit& error) { return error.status; } catch (const Signal& signal) {
  return 128 + signal.number;
 } catch (const std::exception& error) {
  log(error.what());
  return 1;
 }
}
