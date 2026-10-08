#include "src/frameworks/transport/browser_record_ring.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <utility>
#include <vector>
namespace mmltk::frameworks::transport {
namespace {
[[nodiscard]] BrowserOutputRecord record(const BrowserRecordPriority priority = BrowserRecordPriority::Transient) { return {.bytes = {std::byte{0x01}}, .priority = priority}; }
TEST_CASE("browser output ring has exactly sixty-four FIFO records", "[frameworks][transport][browser]") {
 BrowserRecordRing ring;
 for (std::size_t index = 0U; index < kBrowserRecordRingCapacity; ++index)
  REQUIRE(ring.push({.bytes = {static_cast<std::byte>(index)}, .priority = BrowserRecordPriority::Transient}) == BrowserRecordPush::Enqueued);
 CHECK(ring.size() == kBrowserRecordRingCapacity);
 CHECK(ring.push(record()) == BrowserRecordPush::Dropped);
 CHECK(ring.push(record(BrowserRecordPriority::Critical)) == BrowserRecordPush::ClosePeer);
 const auto check_next = [&](std::size_t index) {
  const auto popped = ring.pop();
  REQUIRE(popped);
  CHECK(popped->bytes == std::vector<std::byte>{static_cast<std::byte>(index)});
 };
 // Drain half, refill beyond the physical end, then check every identity.
 for (std::size_t index = 0U; index < kBrowserRecordRingCapacity / 2U; ++index) { check_next(index); }
 for (std::size_t index = 0U; index < kBrowserRecordRingCapacity / 2U; ++index)
  REQUIRE(ring.push({.bytes = {static_cast<std::byte>(kBrowserRecordRingCapacity + index)}}) == BrowserRecordPush::Enqueued);
 CHECK(ring.size() == kBrowserRecordRingCapacity);
 for (std::size_t index = kBrowserRecordRingCapacity / 2U; index < kBrowserRecordRingCapacity * 3U / 2U; ++index) { check_next(index); }
 CHECK(ring.empty());
}
TEST_CASE("latest complete state replaces pending state after ordered edges", "[frameworks][transport][browser]") {
 BrowserRecordRing ring;
 auto snapshot = record(BrowserRecordPriority::Critical);
 snapshot.state_system = 7U;
 snapshot.state_event = 11U;
 REQUIRE(ring.push(snapshot) == BrowserRecordPush::Enqueued);
 for (std::size_t index = 1U; index < kBrowserRecordRingCapacity; ++index) REQUIRE(ring.push(record(BrowserRecordPriority::Critical)) == BrowserRecordPush::Enqueued);
 snapshot.bytes[0] = std::byte{0x02};
 snapshot.state_revision = 2U;
 BrowserRecordObservation observation;
 REQUIRE(ring.push(snapshot, &observation) == BrowserRecordPush::Enqueued);
 CHECK(observation.coalesced);
 CHECK_FALSE(observation.stale);
 CHECK(observation.occupancy == kBrowserRecordRingCapacity);
 auto stale = snapshot;
 stale.state_revision = 1U;
 stale.bytes[0] = std::byte{0x01};
 REQUIRE(ring.push(std::move(stale), &observation) == BrowserRecordPush::Enqueued);
 CHECK(observation.stale);
 CHECK_FALSE(observation.coalesced);
 CHECK(observation.occupancy == kBrowserRecordRingCapacity);
 CHECK(ring.size() == kBrowserRecordRingCapacity);
 for (std::size_t index = 1U; index < kBrowserRecordRingCapacity; ++index) {
  const auto edge = ring.pop();
  REQUIRE(edge);
  CHECK(edge->state_system == 0U);
 }
 const auto latest = ring.pop();
 REQUIRE(latest);
 CHECK(latest->bytes == snapshot.bytes);
 CHECK(latest->state_event == snapshot.state_event);
 CHECK(ring.empty());
}
TEST_CASE("distinct latest state requires capacity or a fresh bootstrap", "[frameworks][transport][browser]") {
 BrowserRecordRing ring;
 for (std::size_t index = 0U; index < kBrowserRecordRingCapacity; ++index) REQUIRE(ring.push(record()) == BrowserRecordPush::Enqueued);
 auto snapshot = record(BrowserRecordPriority::Critical);
 snapshot.state_system = 7U;
 snapshot.state_event = 11U;
 CHECK(ring.push(snapshot) == BrowserRecordPush::ClosePeer);
}
}  // namespace
}  // namespace mmltk::frameworks::transport
