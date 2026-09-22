#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/types.h>
#include "src/controller/services/firefox_process_observation.h"
#include "src/controller/services/runtime_diagnostics.h"
namespace mmltk::test {
class FirefoxProcessOwnerTestAccess;
}
namespace mmltk::controller::services {
struct FirefoxProcessConfig final {
 std::filesystem::path executable;
 std::string page_url;
 std::filesystem::path log_file{};
 bool integration = false;
 bool integration_high_dpi = false;
 std::chrono::milliseconds stop_grace{std::chrono::seconds{5}};
};
struct FirefoxProcessObservationTarget final {
 void* context = nullptr;
 bool (*install_process_group)(void*, pid_t) noexcept = nullptr;
 void (*submit_observation)(void*, FirefoxPhysicalObservation) noexcept = nullptr;
 [[nodiscard]] bool install(const pid_t process_group) const noexcept { return install_process_group != nullptr && install_process_group(context, process_group); }
 void publish(const FirefoxPhysicalObservation observation) const noexcept {
  if (submit_observation != nullptr) submit_observation(context, observation);
 }
};
enum class FirefoxProcessStartResult : unsigned char {
 ChildInstalled,
 Terminal,
};
class FirefoxProcessOwner final {
public:
 FirefoxProcessOwner(std::filesystem::path workspace_import_socket, FirefoxProcessConfig config, FirefoxProcessObservationTarget observations, RuntimeDiagnosticTarget diagnostics = {}) noexcept;
 ~FirefoxProcessOwner() noexcept;
 FirefoxProcessOwner(const FirefoxProcessOwner&) = delete;
 FirefoxProcessOwner& operator=(const FirefoxProcessOwner&) = delete;
 FirefoxProcessOwner(FirefoxProcessOwner&&) = delete;
 FirefoxProcessOwner& operator=(FirefoxProcessOwner&&) = delete;
 [[nodiscard]] FirefoxProcessStartResult start() noexcept;
 void request_stop() noexcept;
 void wait() noexcept;
 [[nodiscard]] FirefoxProcessLifecycle lifecycle() const noexcept;

private:
 explicit FirefoxProcessOwner(FirefoxProcessObservationTarget observations) noexcept;
 void prefer_retained_pid_wait_for_test() noexcept;
 class Implementation;
 FirefoxProcessObservationTarget observations_{};
 FirefoxProcessLifecycle fallback_lifecycle_{.terminal = FirefoxProcessTerminal::StartupFailed};
 bool fallback_published_ = false;
 std::unique_ptr<Implementation> implementation_;
 friend class ::mmltk::test::FirefoxProcessOwnerTestAccess;
};
[[nodiscard]] bool block_browser_runtime_signals() noexcept;
[[nodiscard]] int browser_runtime_exit_status(FirefoxProcessLifecycle firefox, bool application_healthy) noexcept;
}  // namespace mmltk::controller::services
