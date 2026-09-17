#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/acceptance/tests/workflow_wayland_inputs.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/common/io/scoped_fd.h"
#include "artifact_cursor.h"
#include "surface_audit.h"
#include "pixel_audit.h"
#include "native_audit.h"
#include "browser_audit.h"
namespace mmltk::acceptance::wayland {
using mmltk::common::io::ScopedFd;
using mmltk::testsupport::ScopedTempDir;
enum class TerminationMode : std::uint8_t {
    SignalInterrupt,
    WindowClose,
    AbruptPeerLoss,
};
[[nodiscard]] constexpr std::string_view termination_label(const TerminationMode mode) noexcept {
    switch (mode) {
        case TerminationMode::SignalInterrupt: return "sigint";
        case TerminationMode::WindowClose: return "window-close";
        case TerminationMode::AbruptPeerLoss: return "peer-loss";
    }
    std::terminate();
}
class ArtifactNotifications;
class BrowserHostProcess;
class PreparedWaylandInputs final {
   public:
    PreparedWaylandInputs();
    [[nodiscard]] const auto& square() const noexcept { return square_; }
    [[nodiscard]] const auto& mixed() const noexcept { return mixed_; }
    [[nodiscard]] const mmltk::testsupport::WorkflowWaylandInputs& workflows();
    [[nodiscard]] const mmltk::backend::data::testsupport::FixtureSpec& probe();

   private:
    ScopedTempDir root_;
    mmltk::backend::data::testsupport::FixtureSpec square_;
    mmltk::backend::data::testsupport::FixtureSpec mixed_;
    mmltk::backend::data::testsupport::FixtureSpec probe_;
    std::unique_ptr<mmltk::testsupport::WorkflowWaylandInputs> workflows_;
};
// One owner retains the process, resources, physical evidence and byte cursors.
// RunScenario contains domain assertions; Advance is acknowledged only after
// typed UI settlement and those independent evidence streams have joined.
class WaylandSession final {
   public:
    WaylandSession(std::shared_ptr<PreparedWaylandInputs> inputs, TerminationMode terminal, std::string profile, bool diagnostics_enabled = true,
                   bool dpi = false, bool host_to_device = true, std::string fault = {}, bool pixels_enabled = true,
                   const mmltk::backend::data::testsupport::FixtureSpec* ordinary_fixture = nullptr);
    ~WaylandSession();
    void RunScenario(const std::string& viewer_scenario, bool last, bool dark = false, bool pending_reconstruction = false);

   private:
    void AdvanceScenario();
    void ConsumeRecords(bool final = false);
    void RunWorkflows();
    std::shared_ptr<PreparedWaylandInputs> inputs_;
    const mmltk::backend::data::testsupport::FixtureSpec& ordinary_fixture_;
    TerminationMode termination;
    std::string profile_;
    bool logging;
    bool high_dpi;
    bool h2d;
    bool pixel_probes;
    bool pixel_fixture;
    std::string probe_failure;
    ScopedTempDir quiet_artifacts{"mmltk-wayland-quiet-artifacts"};
    ScopedTempDir working{"mmltk-workspace-wayland-session"};
    std::filesystem::path ordinary_compiled_;
    std::filesystem::path diagnostics;
    std::filesystem::path runtime_log;
    std::filesystem::path firefox_log;
    std::filesystem::path acceptance_log;
    std::unique_ptr<ArtifactNotifications> notifications_;
    std::unique_ptr<BrowserHostProcess> process_;
    ScopedFd deadline;
    NativeAudit native;
    BrowserAudit browser;
    SurfaceAudit surface_audit;
    PixelBoundaryAudit pixel_audit;
    std::unique_ptr<JsonLineCursor> native_cursor_;
    std::unique_ptr<JsonLineCursor> browser_cursor_;
    std::uint64_t scenario_sequence_ = 1U;
    bool frontend_settled_ = false;
    bool pressure_entered_ = false;
    bool browser_failed_ = false;
    std::set<std::string> workflow_steps_;
    std::set<std::string> workflow_pixels_;
};
[[nodiscard]] std::shared_ptr<PreparedWaylandInputs> wayland_inputs(bool require_compiled);
[[nodiscard]] bool execution_requested() noexcept;
[[nodiscard]] bool gdr_transport_available() noexcept;
[[nodiscard]] std::filesystem::path configured_path(const char* const name, const std::filesystem::path& fallback);
[[nodiscard]] std::filesystem::path latest_wayland_artifact(const std::string_view filename);
void prepare_latest_log(const std::filesystem::path& path, const std::string_view description, const std::string& identity);
[[nodiscard]] std::filesystem::path artifact_sibling(const std::filesystem::path& native, const std::string_view suffix);
void require_independent_artifacts(const std::span<const std::filesystem::path> paths);
void rotate_process_log_family(const std::filesystem::path& native, const std::string& identity);
[[nodiscard]] std::string read_from(const std::filesystem::path& path, const std::uintmax_t offset);
[[nodiscard]] std::string read_tail(const std::filesystem::path& path);
[[nodiscard]] std::size_t permitted_cpu_count() noexcept;
[[nodiscard]] std::string process_status_text(const int status);
}  // namespace mmltk::acceptance::wayland
