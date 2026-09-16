#include <catch2/catch_test_macros.hpp>
#include <array>
#include <chrono>
#include <future>
#include <limits>
#include <optional>
#include <stop_token>
#include "src/acceptance/tests/async_test_utils.hpp"
#include "src/controller/subsystems/system/detail/prediction_playback.h"
namespace {
using Playback = mmltk::controller::detail::PredictionPlayback;
using namespace std::chrono_literals;
constexpr auto start = Playback::Clock::time_point{10s};
constexpr auto invalid_timestamp = std::numeric_limits<double>::quiet_NaN();
constexpr auto infinity = std::numeric_limits<double>::infinity();
}  // namespace
TEST_CASE("video cadence uses monotonic presentation timing and honest fallback", "[controller][video]") {
    CHECK(Playback::Interval(1.0, 1.25, 30.0) == 0.25);
    CHECK(Playback::Interval(1.0, 1.0, 4.0) == 0.25);
    CHECK(Playback::Interval({}, {}, 4.0) == 0.25);
    CHECK_THROWS_AS(Playback::Interval(2.0, 1.0, 0.0), std::runtime_error);
    CHECK_THROWS_AS(Playback::Interval({}, {}, invalid_timestamp), std::runtime_error);
    Playback playback;
    std::stop_source stop;
    stop.request_stop();
    playback.Pause(true);
    CHECK_FALSE(playback.Wait(0.0, 30.0, stop.get_token()));
}
TEST_CASE("video scheduling accounts for fallback before timestamp recovery", "[controller][video]") {
    Playback playback;
    CHECK(playback.Schedule(10.0, 4.0, start) == start);
    CHECK(playback.Schedule({}, 4.0, start) == start + 250ms);
    CHECK(playback.Schedule(10.5, 4.0, start) == start + 500ms);
    CHECK(playback.Schedule(10.75, 0.0, start) == start + 750ms);
}
TEST_CASE("video fallback retains the valid cursor through invalid timestamps", "[controller][video]") {
    Playback playback;
    CHECK(playback.Schedule(10.0, 4.0, start) == start);
    const std::array<std::optional<double>, 6> invalid{{{}, invalid_timestamp, infinity, -infinity, 10.0, 1.0}};
    auto expected = start;
    for (const auto timestamp : invalid) {
        expected += 250ms;
        CHECK(playback.Schedule(timestamp, 4.0, start) == expected);
    }
    // The source delta includes all six already-scheduled fallback intervals.
    CHECK(playback.Schedule(11.75, 0.0, start) == start + 1750ms);
    CHECK(playback.Schedule({}, 2.0, start) == start + 2250ms);
    CHECK(playback.Schedule(12.5, 0.0, start) == start + 2500ms);
}
TEST_CASE("video recovery never moves a deadline backward when fallback overtakes PTS", "[controller][video]") {
    for (const double recovered : {10.25, 10.5}) {
        Playback playback;
        CHECK(playback.Schedule(10.0, 4.0, start) == start);
        CHECK(playback.Schedule({}, 4.0, start) == start + 250ms);
        CHECK(playback.Schedule(9.0, 4.0, start) == start + 500ms);
        CHECK(playback.Schedule(recovered, 0.0, start) == start + 500ms);
        CHECK(playback.Schedule(recovered + 0.25, 0.0, start) == start + 750ms);
    }
}
TEST_CASE("video first timing and failed scheduling leave a usable source baseline", "[controller][video]") {
    Playback playback;
    for (const auto timestamp : std::array<std::optional<double>, 3>{{{}, invalid_timestamp, infinity}})
        CHECK_THROWS_AS(playback.Schedule(timestamp, 0.0, start), std::runtime_error);
    CHECK(playback.Schedule({}, 4.0, start) == start);
    CHECK(playback.Schedule(10.0, 4.0, start) == start + 250ms);
    CHECK(playback.Schedule(10.25, 0.0, start) == start + 500ms);
    CHECK_THROWS_AS(playback.Schedule({}, 0.0, start), std::runtime_error);
    CHECK_THROWS_AS(playback.Schedule({}, std::numeric_limits<double>::denorm_min(), start), std::runtime_error);
    CHECK_THROWS_AS(playback.Schedule(1e300, 0.0, start), std::runtime_error);
    CHECK(playback.Schedule(10.5, 0.0, start) == start + 750ms);
    CHECK(playback.Schedule({}, 4.0, start) == start + 1s);
    CHECK_THROWS_AS(playback.Schedule({}, 0.0, start), std::runtime_error);
    CHECK_THROWS_AS(playback.Schedule(1e300, 0.0, start), std::runtime_error);
    CHECK(playback.Schedule(11.0, 0.0, start) == start + 1250ms);
    playback.Reset();
    CHECK(playback.Schedule(-10.0, 0.0, start) == start);
    CHECK(playback.Schedule(-9.75, 0.0, start) == start + 250ms);
}
TEST_CASE("video deadlines account for inference time and rebase overdue frames", "[controller][video]") {
    CHECK(Playback::Advance(start, start + 100ms, 0.5) == start + 500ms);
    const auto late = Playback::Advance(start, start + 2s, 0.5);
    CHECK(late == start + 2s);
    CHECK(Playback::Advance(late, late + 100ms, 0.5) == late + 500ms);
    for (const double seconds : {0.0, -1.0, invalid_timestamp, infinity, 1e300}) CHECK_THROWS_AS(Playback::Advance(start, start, seconds), std::runtime_error);
    const auto limit = Playback::Clock::time_point::max() - 1s;
    CHECK_THROWS_AS(Playback::Advance(limit, limit, 1.0), std::runtime_error);
    Playback playback;
    CHECK(playback.Schedule(10.0, 4.0, start) == start);
    CHECK(playback.Schedule({}, 4.0, start + 2s) == start + 2s);
    CHECK(playback.Schedule(10.5, 4.0, start + 2100ms) == start + 2250ms);
    CHECK(playback.Schedule(10.75, 4.0, start + 2300ms) == start + 2500ms);
    CHECK(playback.Schedule({}, 4.0, start + 4s) == start + 4s);
    CHECK(playback.Schedule(11.0, 4.0, start + 4100ms) == start + 4100ms);
    CHECK(playback.Schedule(11.25, 4.0, start + 4200ms) == start + 4350ms);
}
TEST_CASE("video pause excludes wall time without consuming source fallback credit", "[controller][video]") {
    Playback playback;
    CHECK(playback.Schedule(10.0, 4.0, start) == start);
    CHECK(playback.Schedule({}, 4.0, start) == start + 250ms);
    playback.Pause(true, start + 100ms);
    playback.Pause(true, start + 200ms);
    playback.Pause(false, start + 2100ms);
    playback.Pause(false, start + 2200ms);
    CHECK(playback.Schedule(10.5, 4.0, start + 2200ms) == start + 2500ms);
    CHECK(playback.Schedule(10.75, 4.0, start + 2300ms) == start + 2750ms);
    CHECK(playback.Schedule({}, 4.0, start) == start + 3s);
    playback.Pause(true, start + 2400ms);
    playback.Reset();
    CHECK(playback.Schedule(1.0, 4.0, start) == start);
    CHECK(playback.Schedule(1.25, 0.0, start) == start + 250ms);
}
TEST_CASE("video pause rejects clock overflow without corrupting the pending deadline", "[controller][video]") {
    Playback playback;
    const auto limit = Playback::Clock::time_point::max() - 1s;
    CHECK(playback.Schedule(10.0, 4.0, limit) == limit);
    playback.Pause(true, start);
    CHECK_THROWS_AS(playback.Pause(false, start - 1s), std::runtime_error);
    CHECK_THROWS_AS(playback.Pause(false, start + 1s), std::runtime_error);
    playback.Pause(false, start + 250ms);
    CHECK(playback.Schedule(10.25, 4.0, limit) == limit + 500ms);
}
TEST_CASE("video live wait shares fallback and recovery scheduling", "[controller][video]") {
    for (const bool recover_in_wait : {false, true}) {
        Playback playback;
        // Keep the pending deadline ahead of the real clock, so cancellation
        // exposes the exact live scheduling transition without timing tolerances.
        const auto future_start = Playback::Clock::now() + 100s;
        REQUIRE(playback.Schedule(10.0, 4.0, future_start) == future_start);
        if (recover_in_wait) REQUIRE(playback.Schedule({}, 4.0, future_start) == future_start + 250ms);
        std::stop_source stop;
        std::promise<void> entered;
        auto waiting = std::async(std::launch::async, [&] {
            entered.set_value();
            return playback.Wait(recover_in_wait ? std::optional{10.5} : std::nullopt, 4.0, stop.get_token());
        });
        mmltk::testsupport::ScopedTestCleanup cleanup([&] { stop.request_stop(); });
        mmltk::testsupport::await_test_promise(entered, "live playback entry");
        CHECK(waiting.wait_for(50ms) == std::future_status::timeout);
        stop.request_stop();
        CHECK_FALSE(mmltk::testsupport::await_test_future(waiting, "live playback cancellation"));
        if (recover_in_wait)
            CHECK(playback.Schedule(10.75, 0.0, future_start) == future_start + 750ms);
        else
            CHECK(playback.Schedule(10.5, 0.0, future_start) == future_start + 500ms);
    }
}
TEST_CASE("video stop interrupts pause and outstanding pacing", "[controller][video]") {
    for (const bool paused : {true, false}) {
        Playback playback;
        REQUIRE(playback.Wait(0.0, 1.0, {}));
        playback.Pause(paused);
        std::stop_source stop;
        std::promise<void> entered;
        auto waiting = std::async(std::launch::async, [&] {
            entered.set_value();
            return playback.Wait(3600.0, 1.0, stop.get_token());
        });
        mmltk::testsupport::ScopedTestCleanup cleanup([&] { stop.request_stop(); });
        mmltk::testsupport::await_test_promise(entered, "playback stop entry");
        CHECK(waiting.wait_for(50ms) == std::future_status::timeout);
        stop.request_stop();
        CHECK_FALSE(mmltk::testsupport::await_test_future(waiting, "playback stop"));
        playback.Reset();
        CHECK(playback.Wait(0.0, 1.0, {}));
    }
}
TEST_CASE("video resume excludes time spent paused from its pending frame", "[controller][video]") {
    Playback playback;
    REQUIRE(playback.Wait(0.0, 1.0, {}));
    playback.Pause(true);
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] { return playback.Wait(1.0, 1.0, stop.get_token()); });
    mmltk::testsupport::ScopedTestCleanup cleanup([&] { stop.request_stop(); });
    CHECK(waiting.wait_for(50ms) == std::future_status::timeout);
    playback.Pause(false);
    CHECK(waiting.wait_for(50ms) == std::future_status::timeout);
    stop.request_stop();
    CHECK_FALSE(mmltk::testsupport::await_test_future(waiting, "resumed playback stop"));
}
