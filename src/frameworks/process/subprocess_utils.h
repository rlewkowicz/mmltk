#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "src/common/io/scoped_fd.h"
namespace mmltk::frameworks::process {
enum class ChildSetupStage : std::uint8_t {
    SetProcessGroup,
    RedirectOutput,
    RedirectErrors,
    PreserveDescriptor,
    SetEnvironment,
    Exec,
};
[[nodiscard]] const char* child_setup_stage_label(ChildSetupStage stage) noexcept;
struct ChildSetupFailure {
    ChildSetupStage stage{};
    int error_number = 0;
};
struct CapturedChildProcessResult {
    std::string output;
    int status = -1;
    std::optional<ChildSetupFailure> setup_failure;
    bool output_limit_exceeded = false;
};
enum class CapturedChildAbortReason : std::uint8_t { Cancelled, TimedOut };
struct ChildCancellationTarget final {
    void* context = nullptr;
    bool (*requested)(void*) noexcept = nullptr;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool is_requested() const noexcept;
};
class CapturedChildAborted final : public std::runtime_error {
   public:
    CapturedChildAborted(const std::string& message, CapturedChildAbortReason reason);
    [[nodiscard]] CapturedChildAbortReason reason() const noexcept;

   private:
    CapturedChildAbortReason reason_;
};
class CapturedChildProcess final {
   public:
    CapturedChildProcess() = default;
    CapturedChildProcess(int pid, int pidfd, int stdout_fd, int setup_error_fd) noexcept;
    CapturedChildProcess(const CapturedChildProcess&) = delete;
    CapturedChildProcess& operator=(const CapturedChildProcess&) = delete;
    CapturedChildProcess(CapturedChildProcess&& other) noexcept;
    CapturedChildProcess& operator=(CapturedChildProcess&& other) noexcept;
    ~CapturedChildProcess();
    [[nodiscard]] int pid() const noexcept;
    [[nodiscard]] int pidfd() const noexcept;
    [[nodiscard]] int stdout_fd() const noexcept;
    [[nodiscard]] int setup_error_fd() const noexcept;
    [[nodiscard]] int release_pid() noexcept;
    [[nodiscard]] int release_pidfd() noexcept;
    [[nodiscard]] int release_stdout_fd() noexcept;
    [[nodiscard]] int release_setup_error_fd() noexcept;
    void close_output() noexcept;
    void close() noexcept;

   private:
    int pid_ = -1;
    mmltk::common::io::ScopedFd pidfd_;
    mmltk::common::io::ScopedFd stdout_fd_;
    mmltk::common::io::ScopedFd setup_error_fd_;
};
struct ChildSetupTarget final {
    void* context = nullptr;
    void (*invoke)(void*, int, int) = nullptr;
};
[[nodiscard]] CapturedChildProcess spawn_captured_child_process_erased(std::string_view process_name, ChildSetupTarget child_setup, bool nonblocking_output);
[[nodiscard]] CapturedChildProcessResult run_captured_child_process_erased(std::string_view process_name, std::string_view output_error_prefix,
                                                                           ChildSetupTarget child_setup, ChildCancellationTarget cancellation,
                                                                           std::chrono::milliseconds timeout, int cancel_fd, bool kill_process_group,
                                                                           std::size_t output_limit);
[[noreturn]] void fail_child_setup(int setup_fd, ChildSetupStage stage) noexcept;
void require_child_setup_step(bool succeeded, int setup_fd, ChildSetupStage stage) noexcept;
void prepare_captured_output_child(int output_fd, int setup_fd) noexcept;
[[nodiscard]] std::optional<ChildSetupFailure> read_child_setup_failure(int fd);
[[nodiscard]] std::string format_child_setup_failure(const ChildSetupFailure& failure, std::string_view process_name);
[[nodiscard]] int wait_child_process(int pid);
class ArgvBuffer final {
   public:
    explicit ArgvBuffer(std::vector<std::string> arguments);
    ~ArgvBuffer();
    ArgvBuffer(const ArgvBuffer&) = delete;
    ArgvBuffer& operator=(const ArgvBuffer&) = delete;
    [[nodiscard]] char* const* data() const noexcept;
    [[nodiscard]] char* program() const noexcept;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
inline constexpr std::size_t kCapturedChildReadBudget = std::size_t{64U} * 1024U;
inline constexpr std::size_t kMaximumCapturedChildOutputBytes = std::size_t{8U} * 1024U * 1024U;
template <typename ChildSetupFn>
CapturedChildProcess spawn_captured_child_process(const std::string_view process_name, ChildSetupFn&& setup, const bool nonblocking_output = true) {
    using Setup = std::remove_reference_t<ChildSetupFn>;
    ChildSetupTarget target{std::addressof(setup), [](void* context, int output, int errors) { std::invoke(*static_cast<Setup*>(context), output, errors); }};
    return spawn_captured_child_process_erased(process_name, target, nonblocking_output);
}
template <typename ChildSetupFn>
CapturedChildProcessResult run_captured_child_process(const std::string_view process_name, const std::string_view output_error_prefix, ChildSetupFn&& setup,
                                                      const ChildCancellationTarget cancellation = {},
                                                      const std::chrono::milliseconds timeout = std::chrono::milliseconds{0}, const int cancel_fd = -1,
                                                      const bool kill_process_group = false, const std::size_t output_limit = kCapturedChildReadBudget) {
    using Setup = std::remove_reference_t<ChildSetupFn>;
    ChildSetupTarget target{std::addressof(setup), [](void* context, int output, int errors) { std::invoke(*static_cast<Setup*>(context), output, errors); }};
    return run_captured_child_process_erased(process_name, output_error_prefix, target, cancellation, timeout, cancel_fd, kill_process_group, output_limit);
}
}  // namespace mmltk::frameworks::process
