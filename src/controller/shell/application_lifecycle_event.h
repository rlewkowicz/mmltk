#pragma once
#include <optional>
#include "src/controller/services/firefox_process_observation.h"
namespace mmltk::controller::shell {
enum class ApplicationLifecycleEventSource : unsigned char {
 Shell,
 FirefoxSignal,
 FirefoxProcess,
 Infrastructure,
};
enum class ApplicationShutdownReason : unsigned char {
 WindowClose,
 FirefoxExit,
 SignalInterrupt,
 SignalTerminate,
 InfrastructureFailure,
};
struct ApplicationLifecycleEvent final {
 ApplicationLifecycleEventSource source = ApplicationLifecycleEventSource::Shell;
 ApplicationShutdownReason reason = ApplicationShutdownReason::WindowClose;
 std::optional<services::FirefoxProcessLifecycle> firefox{};
};
}  // namespace mmltk::controller::shell
