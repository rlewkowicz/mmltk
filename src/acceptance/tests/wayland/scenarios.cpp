#include "session.h"
namespace mmltk::acceptance::wayland {
void workspace_wayland_retained() {
    WaylandSession session{wayland_inputs(false), TerminationMode::SignalInterrupt, "retained"};
    session.RunScenario("square", false, false, true);
    session.RunScenario("", false);
    session.RunScenario("wide", false);
    session.RunScenario("tall", false);
    session.RunScenario("semantics", false);
    session.RunScenario("copy", false);
    session.RunScenario("copy", false, true);
    session.RunScenario("rapid", true);
}
void workspace_wayland_workflows() {
    WaylandSession session{wayland_inputs(false), TerminationMode::SignalInterrupt, "workflows", true, false, true, {}, false};
    session.RunScenario("workflows", true);
}
void workspace_wayland_dpi() {
    WaylandSession session{wayland_inputs(true), TerminationMode::SignalInterrupt, "dpi", true, true};
    session.RunScenario("copy", false, false, true);
    session.RunScenario("copy", false, true);
    session.RunScenario("rapid", true, true);
}
void workspace_wayland_terminal() {
    const auto terminal = GENERATE(TerminationMode::WindowClose, TerminationMode::AbruptPeerLoss);
    WaylandSession session{wayland_inputs(true), terminal, "terminal", true, false, true, {}, terminal != TerminationMode::WindowClose};
    session.RunScenario("terminal", true);
}
void workspace_wayland_probe_recovery() {
    const std::string boundary = GENERATE("allocation", "reset", "begin", "end");
    const auto inputs = wayland_inputs(false);
    WaylandSession session{inputs, TerminationMode::SignalInterrupt, "semantics", true, false, true, boundary, true, &inputs->probe()};
    session.RunScenario("semantics", true);
}
void workspace_wayland_gdr() {
    if (!execution_requested()) SKIP("workspace Wayland integration requires the packaged hardware runner");
    if (!gdr_transport_available()) SKIP("GDR hardware unavailable; focused packaged GDR coverage remains unverified");
    WaylandSession session{wayland_inputs(true), TerminationMode::SignalInterrupt, "wide", true, false, false};
    session.RunScenario("wide", true);
}
void workspace_wayland_quiet() {
    const std::string profile = GENERATE("blocked", "quiet");
    const char* const diagnostics = std::getenv("MMLTK_RUN_WORKSPACE_WAYLAND_QUIET_DIAGNOSTICS");
    const bool logging = diagnostics && std::string_view{diagnostics} == "1";
    WaylandSession session{wayland_inputs(true), TerminationMode::SignalInterrupt, profile, logging, false, true, {}, false};
    session.RunScenario("quiet", true);
}
TEST_CASE("workspace_wayland_retained", "[workspace_hardware][workspace_wayland_integration][retained]") { workspace_wayland_retained(); }
TEST_CASE("workspace_wayland_workflows", "[workspace_hardware][workspace_wayland_integration][workflows]") { workspace_wayland_workflows(); }
TEST_CASE("workspace_wayland_dpi", "[workspace_hardware][workspace_wayland_integration][dpi]") { workspace_wayland_dpi(); }
TEST_CASE("workspace_wayland_terminal", "[workspace_hardware][workspace_wayland_integration][terminal]") { workspace_wayland_terminal(); }
TEST_CASE("workspace_wayland_probe_recovery", "[workspace_hardware][workspace_wayland_integration][probe_recovery]") { workspace_wayland_probe_recovery(); }
TEST_CASE("workspace_wayland_gdr", "[workspace_hardware][workspace_wayland_integration][gdr]") { workspace_wayland_gdr(); }
TEST_CASE("workspace_wayland_quiet", "[workspace_hardware][workspace_wayland_integration][quiet]") { workspace_wayland_quiet(); }

} // namespace mmltk::acceptance::wayland
