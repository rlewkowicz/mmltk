#include "src/controller/services/file_dialog_client.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/io/scoped_fd.h"
namespace mmltk::controller::services {
namespace {
using PolicyText = BoundedText<kFileDialogTextCapacity>;
using PathText = BoundedText<kFileDialogPathStorageCapacity>;
constexpr std::size_t kArgumentCapacity = 8U;
constexpr std::string_view kHostMount{"/host"};
struct FixedOutput final {
 std::array<char, FileDialogClient::kOutputCapacity> bytes{};
 std::size_t size = 0U;
 void append(const char* source, const std::size_t count) {
  if (count > bytes.size() - size) { throw std::runtime_error("file-dialog helper output exceeded capacity"); }
  std::copy_n(source, count, bytes.data() + static_cast<std::ptrdiff_t>(size));
  size += count;
 }
 [[nodiscard]] std::string_view view() const noexcept { return {bytes.data(), size}; }
};
[[nodiscard]] bool append_text(char* const bytes, const std::size_t capacity, std::uint16_t& size, const std::string_view suffix) noexcept {
 const std::string_view prefix{bytes, size};
 if (prefix.empty() || prefix.size() + suffix.size() >= capacity) return false;
 std::copy_n(suffix.data(), suffix.size(), bytes + static_cast<std::ptrdiff_t>(prefix.size()));
 size = static_cast<std::uint16_t>(prefix.size() + suffix.size());
 return true;
}
[[nodiscard]] bool append(PathText& destination, const std::string_view suffix) noexcept {
 const std::string_view prefix = destination.view();
 return !prefix.empty() && append_text(destination.bytes.data(), destination.bytes.size(), destination.size, suffix);
}
[[nodiscard]] bool append(PolicyText& destination, const std::string_view suffix) noexcept {
 const std::string_view prefix = destination.view();
 return !prefix.empty() && append_text(destination.bytes.data(), destination.bytes.size(), destination.size, suffix);
}
[[nodiscard]] FileDialogResult dialog_result(const FileDialogDisposition disposition, const std::string_view path = {}, const std::string_view error = {},
                                             const FileDialogFailure failure = FileDialogFailure::None) noexcept {
 return {.disposition = disposition,
         .failure = failure,
         .path = path.empty() ? PathText{} : PathText::From(path),
         .error = error.empty() ? PolicyText{} : PolicyText::From(error)};
}
void terminate_and_reap(const pid_t child) noexcept {
 if (child <= 0) return;
 (void)::kill(-child, SIGKILL);
 (void)::kill(child, SIGKILL);
 while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
}
[[nodiscard]] int reap_child(const pid_t child) {
 int status = 0;
 while (::waitpid(child, &status, 0) < 0) {
  if (errno != EINTR) { throw std::runtime_error("file-dialog child reap failed: " + std::string{std::strerror(errno)}); }
 }
 return status;
}
[[nodiscard]] std::filesystem::path canonical_path(const std::filesystem::path& path) {
 std::error_code error;
 std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
 if (error) resolved = std::filesystem::absolute(path, error);
 if (error) { throw std::runtime_error("file-dialog path resolution failed: " + error.message()); }
 return resolved.lexically_normal();
}
[[nodiscard]] bool path_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
 const std::filesystem::path relative = candidate.lexically_relative(root);
 if (relative.empty()) return candidate == root;
 const auto first = relative.begin();
 return !relative.is_absolute() && first != relative.end() && *first != "..";
}
struct DialogScope final {
 std::filesystem::path container_root;
 std::filesystem::path helper_root;
 bool host_mounted = false;
};
[[nodiscard]] DialogScope make_scope(const PathText& launch_directory) {
 DialogScope scope{.container_root = canonical_path(std::filesystem::path{std::string{launch_directory.view()}}), .helper_root = {}, .host_mounted = false};
 const std::filesystem::path host_mount{kHostMount};
 const std::filesystem::path relative = scope.container_root.lexically_relative(host_mount);
 scope.host_mounted = !relative.empty() && !relative.is_absolute() && relative.begin() != relative.end() && *relative.begin() != "..";
 scope.helper_root = scope.host_mounted ? std::filesystem::path{"/"} / relative : scope.container_root;
 return scope;
}
[[nodiscard]] PathText directory_argument(const std::filesystem::path& directory) {
 PathText value = PathText::From(directory.string());
 if (!value.valid()) throw std::runtime_error("file-dialog launch directory exceeds capacity");
 if (!value.view().ends_with('/') && !append(value, "/")) { throw std::runtime_error("file-dialog launch directory exceeds capacity"); }
 return value;
}
[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
 const auto whitespace = [](const unsigned char character) { return character == ' ' || character == '\t' || character == '\r' || character == '\n'; };
 std::size_t first = 0U;
 while (first < value.size() && whitespace(static_cast<unsigned char>(value[first]))) ++first;
 std::size_t last = value.size();
 while (last > first && whitespace(static_cast<unsigned char>(value[last - 1U]))) --last;
 return value.substr(first, last - first);
}
[[nodiscard]] PathText selected_path(const FixedOutput& output, const DialogScope& scope) {
 const std::string_view selected = trim(output.view());
 if (selected.empty()) return {};
 if (selected.find('\0') != std::string_view::npos) { throw std::runtime_error("file-dialog helper returned a path containing a null byte"); }
 std::filesystem::path picked{std::string{selected}};
 if (picked.is_relative()) picked = scope.helper_root / picked;
 std::filesystem::path resolved = canonical_path(picked);
 if (!path_within(scope.container_root, resolved) && scope.host_mounted && picked.is_absolute()) {
  resolved = canonical_path(std::filesystem::path{kHostMount} / picked.relative_path());
 }
 if (!path_within(scope.container_root, resolved)) { throw std::runtime_error("selected path escapes file-dialog launch directory"); }
 PathText result = PathText::From(resolved.string());
 if (!result.valid()) throw std::runtime_error("selected file-dialog path exceeds capacity");
 return result;
}
[[nodiscard]] bool executable_program(const PathText& helper) noexcept {
 struct stat status{};
 return helper.valid() && ::stat(helper.bytes.data(), &status) == 0 && S_ISREG(status.st_mode) && ::access(helper.bytes.data(), X_OK) == 0;
}
[[nodiscard]] PathText resolve_helper_program(const PathText& configured) {
 if (configured.view().contains('/')) return executable_program(configured) ? configured : PathText{};
 const char* environment = std::getenv("PATH");
 if (environment == nullptr) return {};
 std::string_view remaining{environment};
 while (true) {
  const std::size_t separator = remaining.find(':');
  const std::string_view directory = remaining.substr(0U, separator);
  PathText candidate = PathText::From(directory.empty() ? std::string_view{"."} : directory);
  if (candidate.valid() && append(candidate, "/") && append(candidate, configured.view()) && executable_program(candidate)) { return candidate; }
  if (separator == std::string_view::npos) return {};
  remaining.remove_prefix(separator + 1U);
 }
}
[[nodiscard]] bool build_arguments(const PathText& helper, const FileDialogRequest& request, const DialogScope& scope,
                                   std::array<PathText, kArgumentCapacity>& arguments, std::size_t& count) {
 if (!helper.valid() || !request.valid()) return false;
 const auto add = [&](const std::string_view value) {
  if (count == arguments.size()) return false;
  arguments[count] = PathText::From(value);
  if (!arguments[count].valid()) return false;
  ++count;
  return true;
 };
 if (!add(helper.view()) || !add("--file-selection")) return false;
 PathText title = PathText::From("--title=");
 if (!append(title, request.title.view()) || !add(title.view())) return false;
 PathText filename = PathText::From("--filename=");
 const PathText directory = directory_argument(scope.helper_root);
 if (!append(filename, directory.view()) || !add(filename.view())) return false;
 switch (request.mode) {
  case mmltk::controller::contracts::FileDialogMode::OpenFile: break;
  case mmltk::controller::contracts::FileDialogMode::OpenFolder:
   if (!add("--directory")) return false;
   break;
  case mmltk::controller::contracts::FileDialogMode::SaveFile:
   if (!add("--save") || !add("--confirm-overwrite")) return false;
   break;
 }
 PathText filter = PathText::From("--file-filter=");
 if (!append(filter, request.filter.name.view()) || !append(filter, " | ") || !append(filter, request.filter.pattern.view()) || !add(filter.view())) {
  return false;
 }
 return true;
}
enum class ChildSetupStage : std::uint8_t {
 ProcessGroup,
 RedirectOutput,
 RedirectErrors,
 Exec,
};
struct ChildSetupFailure final {
 ChildSetupStage stage{};
 int error_number = 0;
};
[[noreturn]] void fail_child_setup(const int setup_descriptor, const ChildSetupStage stage) noexcept {
 const ChildSetupFailure failure{.stage = stage, .error_number = errno};
 const auto* bytes = reinterpret_cast<const char*>(&failure);
 std::size_t offset = 0U;
 while (offset < sizeof(failure)) {
  const ssize_t written = ::write(setup_descriptor, bytes + offset, sizeof(failure) - offset);
  if (written <= 0) break;
  offset += static_cast<std::size_t>(written);
 }
 _exit(127);
}
[[nodiscard]] std::string_view setup_stage_name(const ChildSetupStage stage) noexcept {
 switch (stage) {
  case ChildSetupStage::ProcessGroup: return "process group";
  case ChildSetupStage::RedirectOutput: return "output redirect";
  case ChildSetupStage::RedirectErrors: return "error redirect";
  case ChildSetupStage::Exec: return "exec";
 }
 return "unknown";
}
struct ChildSetupError final {
 PolicyText message{};
 FileDialogFailure failure = FileDialogFailure::None;
 [[nodiscard]] bool valid() const noexcept { return message.valid() && failure != FileDialogFailure::None; }
};
[[nodiscard]] ChildSetupError setup_error(const mmltk::common::io::ScopedFd& descriptor) {
 ChildSetupFailure failure{};
 std::size_t offset = 0U;
 while (offset < sizeof(failure)) {
  const ssize_t bytes = ::read(descriptor.get(), reinterpret_cast<char*>(&failure) + offset, sizeof(failure) - offset);
  if (bytes > 0) {
   offset += static_cast<std::size_t>(bytes);
   continue;
  }
  if (bytes < 0 && errno == EINTR) continue;
  break;
 }
 if (offset != sizeof(failure)) return {};
 PolicyText message = PolicyText::From("file-dialog helper ");
 if (!append(message, setup_stage_name(failure.stage)) || !append(message, " failed: ") || !append(message, std::strerror(failure.error_number))) {
  message = PolicyText::From("file-dialog helper setup failed");
 }
 return {.message = message, .failure = failure.stage == ChildSetupStage::Exec ? FileDialogFailure::Exec : FileDialogFailure::ChildSetup};
}
enum class OutputDrainState : std::uint8_t { Pending, Closed };
[[nodiscard]] OutputDrainState drain_output_available(mmltk::common::io::ScopedFd& descriptor, FixedOutput& output) {
 if (descriptor.get() < 0) return OutputDrainState::Closed;
 std::array<char, 4096U> buffer{};
 while (true) {
  const ssize_t bytes = ::read(descriptor.get(), buffer.data(), buffer.size());
  if (bytes > 0) {
   output.append(buffer.data(), static_cast<std::size_t>(bytes));
   continue;
  }
  if (bytes < 0 && errno == EINTR) continue;
  if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return OutputDrainState::Pending; }
  if (bytes == 0) {
   descriptor.reset();
   return OutputDrainState::Closed;
  }
  throw std::runtime_error("file-dialog output read failed: " + std::string{std::strerror(errno)});
 }
}
void drain_output_to_eof(mmltk::common::io::ScopedFd& descriptor, FixedOutput& output) {
 while (drain_output_available(descriptor, output) != OutputDrainState::Closed) {
  pollfd event{.fd = descriptor.get(), .events = POLLIN | POLLHUP, .revents = 0};
  int ready = -1;
  do { ready = ::poll(&event, 1U, -1); } while (ready < 0 && errno == EINTR);
  if (ready < 0 || (event.revents & POLLNVAL) != 0) { throw std::runtime_error("file-dialog output drain failed: " + std::string{std::strerror(errno)}); }
 }
}
}  // namespace
struct FileDialogClient::Configuration final {
 PathText helper{};
 PathText launch_directory{};
};
FileDialogClientOwner::FileDialogClientOwner(const std::string_view helper_program, const std::string_view launch_directory) {
 const PathText helper = PathText::From(helper_program);
 const PathText launch = PathText::From(launch_directory);
 if (!helper.valid() || !launch.valid()) { throw std::invalid_argument("file-dialog owner configuration is invalid"); }
 client_ = FileDialogClient{std::make_shared<const FileDialogClient::Configuration>(
  FileDialogClient::Configuration{.helper = resolve_helper_program(helper), .launch_directory = launch})};
}
FileDialogClient FileDialogClientOwner::client() const noexcept { return client_.valid() ? client_ : FileDialogClient{}; }
bool FileDialogClient::valid() const noexcept { return configuration_ && configuration_->helper.valid() && configuration_->launch_directory.valid(); }
FileDialogResult FileDialogClient::run(const FileDialogRequest& request, FileDialogCancellationToken cancellation) const noexcept {
 pid_t child = -1;
 try {
  const auto configuration = configuration_;
  if (!configuration || !request.valid()) {
   return dialog_result(FileDialogDisposition::Failed, {}, configuration ? "invalid file-dialog request" : "file-dialog capability is unavailable",
                        configuration ? FileDialogFailure::InvalidRequest : FileDialogFailure::CapabilityUnavailable);
  }
  mmltk::common::io::ScopedFd cancellation_descriptor{cancellation.release()};
  if (cancellation_descriptor.get() < 0 || mmltk::common::concurrency::detail::cancellation_descriptor_requested(cancellation_descriptor.get())) {
   return dialog_result(FileDialogDisposition::Reaped);
  }
  const DialogScope scope = make_scope(configuration->launch_directory);
  const PathText& helper = configuration->helper;
  std::array<PathText, kArgumentCapacity> arguments{};
  std::size_t argument_count = 0U;
  if (!build_arguments(helper, request, scope, arguments, argument_count)) {
   return dialog_result(FileDialogDisposition::Failed, {}, "file-dialog argument exceeded capacity", FileDialogFailure::InvalidRequest);
  }
  std::array<char*, kArgumentCapacity + 1U> arguments_view{};
  for (std::size_t index = 0U; index < argument_count; ++index) { arguments_view[index] = arguments[index].bytes.data(); }
  int output_pipe[2]{-1, -1};
  int setup_pipe[2]{-1, -1};
  if (::pipe2(output_pipe, O_CLOEXEC | O_NONBLOCK) != 0 || ::pipe2(setup_pipe, O_CLOEXEC) != 0) {
   const int error = errno;
   if (output_pipe[0] >= 0) (void)::close(output_pipe[0]);
   if (output_pipe[1] >= 0) (void)::close(output_pipe[1]);
   if (setup_pipe[0] >= 0) (void)::close(setup_pipe[0]);
   if (setup_pipe[1] >= 0) (void)::close(setup_pipe[1]);
   return dialog_result(FileDialogDisposition::Failed, {}, std::strerror(error), FileDialogFailure::PipeCreation);
  }
  mmltk::common::io::ScopedFd output_read{output_pipe[0]};
  mmltk::common::io::ScopedFd output_write{output_pipe[1]};
  mmltk::common::io::ScopedFd setup_read{setup_pipe[0]};
  mmltk::common::io::ScopedFd setup_write{setup_pipe[1]};
  const int child_output_read = output_read.get();
  const int child_output_write = output_write.get();
  const int child_setup_read = setup_read.get();
  const int child_setup_write = setup_write.get();
  child = ::fork();
  if (child < 0) { return dialog_result(FileDialogDisposition::Failed, {}, std::strerror(errno), FileDialogFailure::Fork); }
  if (child == 0) {
   (void)::close(child_output_read);
   (void)::close(child_setup_read);
   if (::setpgid(0, 0) != 0) { fail_child_setup(child_setup_write, ChildSetupStage::ProcessGroup); }
   if (::dup2(child_output_write, STDOUT_FILENO) < 0) { fail_child_setup(child_setup_write, ChildSetupStage::RedirectOutput); }
   const int output_flags = ::fcntl(STDOUT_FILENO, F_GETFL);
   if (output_flags < 0 || ::fcntl(STDOUT_FILENO, F_SETFL, output_flags & ~O_NONBLOCK) != 0) {
    fail_child_setup(child_setup_write, ChildSetupStage::RedirectOutput);
   }
   const int null_descriptor = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
   if (null_descriptor < 0 || ::dup2(null_descriptor, STDERR_FILENO) < 0) { fail_child_setup(child_setup_write, ChildSetupStage::RedirectErrors); }
   (void)::close(null_descriptor);
   (void)::close(child_output_write);
   ::execve(arguments_view[0], arguments_view.data(), ::environ);
   fail_child_setup(child_setup_write, ChildSetupStage::Exec);
  }
  output_write.reset();
  setup_write.reset();
  mmltk::common::io::ScopedFd child_pidfd{static_cast<int>(::syscall(SYS_pidfd_open, child, 0U))};
  if (child_pidfd.get() < 0) {
   terminate_and_reap(child);
   child = -1;
   return dialog_result(FileDialogDisposition::Failed, {}, std::strerror(errno), FileDialogFailure::ProcessHandle);
  }
  FixedOutput output{};
  bool child_exited = false;
  while (!child_exited) {
   std::array<pollfd, 3U> descriptors{{
    {output_read.get(), POLLIN | POLLHUP, 0},
    {child_pidfd.get(), POLLIN, 0},
    {cancellation_descriptor.get(), POLLIN, 0},
   }};
   int ready = -1;
   do { ready = ::poll(descriptors.data(), descriptors.size(), -1); } while (ready < 0 && errno == EINTR);
   if (ready < 0) { throw std::runtime_error("file-dialog wait failed: " + std::string{std::strerror(errno)}); }
   if ((descriptors[2].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
    terminate_and_reap(child);
    child = -1;
    return dialog_result(FileDialogDisposition::Reaped);
   }
   if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) { static_cast<void>(drain_output_available(output_read, output)); }
   child_exited = (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
  }
  (void)::kill(-child, SIGKILL);
  const int status = reap_child(child);
  child = -1;
  drain_output_to_eof(output_read, output);
  if (mmltk::common::concurrency::detail::cancellation_descriptor_requested(cancellation_descriptor.get())) {
   return dialog_result(FileDialogDisposition::Reaped);
  }
  const ChildSetupError child_setup_error = setup_error(setup_read);
  if (child_setup_error.valid()) { return dialog_result(FileDialogDisposition::Failed, {}, child_setup_error.message.view(), child_setup_error.failure); }
  if (!WIFEXITED(status)) {
   return dialog_result(FileDialogDisposition::Failed, {}, "file-dialog helper terminated by signal", FileDialogFailure::ProcessExit);
  }
  const int exit_code = WEXITSTATUS(status);
  if (exit_code == 1) return dialog_result(FileDialogDisposition::Cancelled);
  if (exit_code != 0) { return dialog_result(FileDialogDisposition::Failed, {}, "file-dialog helper rejected request", FileDialogFailure::ProcessExit); }
  const PathText path = selected_path(output, scope);
  return path.valid() ? FileDialogResult{.disposition = FileDialogDisposition::Selected, .failure = FileDialogFailure::None, .path = path}
                      : dialog_result(FileDialogDisposition::Cancelled);
 } catch (const std::exception& error) {
  terminate_and_reap(child);
  return dialog_result(FileDialogDisposition::Failed, {}, error.what(), FileDialogFailure::Unexpected);
 } catch (...) {
  terminate_and_reap(child);
  return dialog_result(FileDialogDisposition::Failed, {}, "file-dialog failed unexpectedly", FileDialogFailure::Unexpected);
 }
}
}  // namespace mmltk::controller::services
