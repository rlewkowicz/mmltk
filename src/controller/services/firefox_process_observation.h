#pragma once

namespace mmltk::controller::services {

enum class FirefoxProcessTerminal : unsigned char {
    Pending,
    Exited,
    Signaled,
    StartupFailed,
};

struct FirefoxProcessLifecycle final {
    FirefoxProcessTerminal terminal = FirefoxProcessTerminal::Pending;
    int status = 1;
    bool stop_requested = false;
    bool kill_selected = false;

    [[nodiscard]] bool settled() const noexcept { return terminal != FirefoxProcessTerminal::Pending; }

    bool operator==(const FirefoxProcessLifecycle&) const = default;
};

enum class FirefoxPhysicalObservationKind : unsigned char {
    ProcessTerminal,
    InfrastructureFailure,
};

struct FirefoxPhysicalObservation final {
    FirefoxPhysicalObservationKind kind = FirefoxPhysicalObservationKind::InfrastructureFailure;
    FirefoxProcessLifecycle process{};
};

}  // namespace mmltk::controller::services
