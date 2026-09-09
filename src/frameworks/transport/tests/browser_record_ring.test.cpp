#include "src/frameworks/transport/browser_record_ring.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>

namespace mmltk::frameworks::transport {
namespace {

[[nodiscard]] BrowserOutputRecord record(const BrowserRecordPriority priority = BrowserRecordPriority::Transient) {
    return {.bytes = {std::byte{0x01}}, .priority = priority};
}

TEST_CASE("browser output ring has exactly sixty-four FIFO records", "[frameworks][transport][browser]") {
    BrowserRecordRing ring;
    for (std::size_t index = 0U; index < kBrowserRecordRingCapacity; ++index)
        REQUIRE(ring.push(record()) == BrowserRecordPush::Enqueued);
    CHECK(ring.size() == kBrowserRecordRingCapacity);
    CHECK(ring.push(record()) == BrowserRecordPush::Dropped);
    CHECK(ring.push(record(BrowserRecordPriority::Critical)) == BrowserRecordPush::ClosePeer);

    auto progress = record(BrowserRecordPriority::Progress);
    progress.bytes.reserve(128U);
    REQUIRE(ring.push(progress) == BrowserRecordPush::Enqueued);
    progress.bytes[0] = std::byte{0x02};
    REQUIRE(ring.push(std::move(progress)) == BrowserRecordPush::Enqueued);
    CHECK(ring.size() == kBrowserRecordRingCapacity + 1U);
    auto consumed = ring.pop();
    REQUIRE(consumed);
    CHECK(consumed->priority == BrowserRecordPriority::Progress);
    CHECK(consumed->bytes.front() == std::byte{0x02});
    ring.recycle(std::move(*consumed));
    CHECK(ring.acquire_progress_storage().capacity() >= 128U);
    for (std::size_t index = 0U; index < kBrowserRecordRingCapacity; ++index)
        REQUIRE(ring.pop());
    CHECK(ring.empty());
}

TEST_CASE("browser output ring reuses wrapped storage", "[frameworks][transport][browser]") {
    BrowserRecordRing ring;
    for (std::size_t index = 0U; index < kBrowserRecordRingCapacity; ++index)
        REQUIRE(ring.push(record()) == BrowserRecordPush::Enqueued);
    for (std::size_t index = 0U; index < kBrowserRecordRingCapacity / 2U; ++index)
        REQUIRE(ring.pop());
    for (std::size_t index = 0U; index < kBrowserRecordRingCapacity / 2U; ++index)
        REQUIRE(ring.push(record()) == BrowserRecordPush::Enqueued);
    CHECK(ring.size() == kBrowserRecordRingCapacity);
}

TEST_CASE("latest complete state replaces pending state after ordered edges", "[frameworks][transport][browser]") {
    BrowserRecordRing ring;
    auto snapshot = record(BrowserRecordPriority::Critical);
    snapshot.state_system = 7U;
    snapshot.state_event = 11U;
    REQUIRE(ring.push(snapshot) == BrowserRecordPush::Enqueued);
    for (std::size_t index = 1U; index < kBrowserRecordRingCapacity; ++index)
        REQUIRE(ring.push(record(BrowserRecordPriority::Critical)) == BrowserRecordPush::Enqueued);
    snapshot.bytes[0] = std::byte{0x02};
    snapshot.state_revision = 2U;
    REQUIRE(ring.push(snapshot) == BrowserRecordPush::Enqueued);
    auto stale = snapshot;
    stale.state_revision = 1U;
    stale.bytes[0] = std::byte{0x01};
    REQUIRE(ring.push(std::move(stale)) == BrowserRecordPush::Enqueued);
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
    for (std::size_t index = 0U; index < kBrowserRecordRingCapacity; ++index)
        REQUIRE(ring.push(record()) == BrowserRecordPush::Enqueued);
    auto snapshot = record(BrowserRecordPriority::Critical);
    snapshot.state_system = 7U;
    snapshot.state_event = 11U;
    CHECK(ring.push(snapshot) == BrowserRecordPush::ClosePeer);
}

}  // namespace
}  // namespace mmltk::frameworks::transport
