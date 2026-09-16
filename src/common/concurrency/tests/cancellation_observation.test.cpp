#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include "src/common/concurrency/cancellation_observation.h"
#include "src/common/concurrency/event_cancellation.h"
namespace {
struct ObservationTestTag;
using EventSource = mmltk::common::concurrency::EventCancellationSource<ObservationTestTag, false>;
TEST_CASE("borrowed cancellation observes atomic and event sources directly", "[common][concurrency]") {
    std::atomic<bool> atomic_source{false};
    const auto atomic = mmltk::common::concurrency::CancellationObservation::Atomic(atomic_source);
    CHECK_FALSE(atomic.requested());
    atomic_source.store(true, std::memory_order_relaxed);
    CHECK(atomic.requested());
    auto [source, token] = EventSource::Mint();
    const auto event = mmltk::common::concurrency::CancellationObservation::Borrow(token);
    CHECK_FALSE(event.requested());
    REQUIRE(source.RequestCancel());
    CHECK(event.requested());
}
}  // namespace
