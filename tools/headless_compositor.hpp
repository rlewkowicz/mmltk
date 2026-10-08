#pragma once

#include "cpp_support.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

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
Environment session_environment(const std::filesystem::path& runtime,
                                Environment environment = current_environment());
int exit_status(int returncode) noexcept;

struct SessionOptions {
    Environment environment = current_environment();
    // Empty paths use environment[XDG_RUNTIME_DIR] and the repository's
    // tools/headless-weston.ini respectively.
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

int run_session(const Command& command, const std::filesystem::path& artifacts,
                SessionOptions options = {});
int main(const Command& arguments);

} // namespace mmltk::tools::headless
