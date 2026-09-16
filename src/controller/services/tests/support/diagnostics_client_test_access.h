#pragma once
#include <mutex>
#include "src/controller/services/diagnostics_client.h"
namespace mmltk::controller::services {
struct DiagnosticsClientTestAccess final {
    [[nodiscard]] static bool WaitForCapacityWaiter(DiagnosticsClient& client) { return client.wait_for_capacity_waiter_for_test(); }
    [[nodiscard]] static std::unique_lock<std::mutex> LockQueue(DiagnosticsClient& client) { return std::unique_lock{client.queue_mutex_for_test()}; }
};
}  // namespace mmltk::controller::services
