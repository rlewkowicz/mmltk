#include "../detail/exclusive_operation_gate.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <semaphore>
#include <thread>

namespace {

using mmltk::common::concurrency::ExclusiveOperationGate;

TEST_CASE("ExclusiveOperationGate rejects a simultaneous owner", "[common][concurrency][exclusive_operation_gate]") {
    ExclusiveOperationGate gate;
    std::binary_semaphore owner_acquired{0};
    std::binary_semaphore release_owner{0};
    std::atomic<bool> owner_held{false};
    std::thread owner([&] {
        auto guard = gate.try_acquire();
        owner_held.store(static_cast<bool>(guard), std::memory_order_release);
        owner_acquired.release();
        release_owner.acquire();
    });
    owner_acquired.acquire();
    REQUIRE(owner_held.load(std::memory_order_acquire));
    REQUIRE(gate.in_flight());
    REQUIRE_FALSE(gate.try_acquire());
    release_owner.release();
    owner.join();
    auto next = gate.try_acquire();
    REQUIRE(next);
}

}  // namespace
