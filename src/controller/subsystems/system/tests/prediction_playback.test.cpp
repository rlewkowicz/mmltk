#include <catch2/catch_test_macros.hpp>
#include <future>
#include <limits>
#include "src/controller/subsystems/system/detail/prediction_playback.h"
TEST_CASE("video cadence uses monotonic presentation timing and honest fallback", "[video]") {
    using Clock = mmltk::controller::detail::PredictionPlayback;
    REQUIRE(Clock::Interval(1.0, 1.25, 30.0) == 0.25);
    REQUIRE(Clock::Interval(1.0, 1.0, 4.0) == 0.25);
    REQUIRE(Clock::Interval({}, {}, 4.0) == 0.25);
    REQUIRE_THROWS_AS(Clock::Interval(2.0, 1.0, 0.0), std::runtime_error);
    Clock clock;
    std::stop_source stop;
    stop.request_stop();
    clock.Pause(true);
    REQUIRE_FALSE(clock.Wait(0.0, 30.0, stop.get_token()));
}

TEST_CASE("video stop interrupts pause and outstanding pacing", "[controller][video]") {
    using Playback = mmltk::controller::detail::PredictionPlayback;
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
        entered.get_future().wait();
        stop.request_stop();
        CHECK(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK_FALSE(waiting.get());
        playback.Reset();
        CHECK(playback.Wait(0.0, 1.0, {}));
    }
    CHECK_THROWS_AS(Playback::Interval({}, {}, std::numeric_limits<double>::quiet_NaN()), std::runtime_error);
}

TEST_CASE("video deadlines account for inference time and rebase overdue frames", "[controller][video]") {
    using Playback = mmltk::controller::detail::PredictionPlayback;
    using namespace std::chrono_literals;
    const auto start = Playback::Clock::time_point{10s};
    CHECK(Playback::Advance(start, start + 100ms, 0.5) == start + 500ms);
    const auto late = Playback::Advance(start, start + 2s, 0.5);
    CHECK(late == start + 2s);
    CHECK(Playback::Advance(late, late + 100ms, 0.5) == late + 500ms);
    CHECK_THROWS_AS(Playback::Advance(start, start, 1e300), std::runtime_error);
}

TEST_CASE("video timestamp fallback preserves the monotonic recovery baseline", "[controller][video]") {
    using Playback = mmltk::controller::detail::PredictionPlayback;
    Playback playback;
    REQUIRE(playback.Wait(10.0, 1000.0, {}));
    REQUIRE(playback.Wait(1.0, 1000.0, {}));
    REQUIRE(playback.Wait({}, 1000.0, {}));
    std::stop_source stop;
    auto recovered = std::async(std::launch::async, [&] { return playback.Wait(10.001, 1000.0, stop.get_token()); });
    const auto ready = recovered.wait_for(std::chrono::seconds(2));
    stop.request_stop();
    CHECK(ready == std::future_status::ready);
    CHECK(recovered.get());
}

TEST_CASE("video resume excludes time spent paused from its pending frame", "[controller][video]") {
    using Playback = mmltk::controller::detail::PredictionPlayback;
    Playback playback;
    REQUIRE(playback.Wait(0.0, 1.0, {}));
    playback.Pause(true);
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] { return playback.Wait(1.0, 1.0, stop.get_token()); });
    CHECK(waiting.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    playback.Pause(false);
    CHECK(waiting.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    stop.request_stop();
    CHECK_FALSE(waiting.get());
}
