#include <atomic>
#include <barrier>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "src/common/concurrency/cancellation_observation.h"
#include "src/common/concurrency/event_cancellation.h"
namespace {
struct ObservationTestTag;
struct OtherObservationTestTag;
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
TEST_CASE("scoped event cancellation preserves borrowing consumption and source policy", "[common][concurrency]") {
    const auto exercise = []<bool CancelOnDestruction>() {
        using Source = mmltk::common::concurrency::EventCancellationSource<ObservationTestTag, CancelOnDestruction>;
        using Binding = mmltk::common::concurrency::ScopedEventCancellation<Source>;
        static_assert(!std::is_copy_constructible_v<Binding> && !std::is_move_constructible_v<Binding>);
        static_assert(!std::is_same_v<typename Binding::Token, mmltk::common::concurrency::EventCancellationToken<OtherObservationTestTag>>);
        static_assert(std::is_same_v<decltype(std::declval<const Binding&>().token()), const typename Source::Token&>);
        {
            std::stop_source stop;
            REQUIRE(stop.request_stop());
            Binding binding{stop.get_token()};
            CHECK(binding.token().cancelled());
            auto token = binding.ConsumeToken();
            CHECK(token.cancelled());
            CHECK_FALSE(binding.token().valid());
        }
        {
            std::stop_source stop;
            Binding binding{stop.get_token()};
            const auto borrowed = mmltk::common::concurrency::CancellationObservation::Borrow(binding.token());
            CHECK_FALSE(borrowed.requested());
            auto token = binding.ConsumeToken();
            CHECK_FALSE(token.cancelled());
            REQUIRE(stop.request_stop());
            CHECK(token.cancelled());
        }
        for (const bool unwind : {false, true}) {
            std::stop_source stop;
            typename Source::Token retained;
            try {
                Binding binding{stop.get_token()};
                retained = binding.ConsumeToken();
                if (unwind) throw std::runtime_error("leave cancellation scope");
            } catch (const std::runtime_error&) {}
            REQUIRE(retained.valid());
            CHECK(retained.cancelled() == CancelOnDestruction);
            REQUIRE(stop.request_stop());
            CHECK(retained.cancelled() == CancelOnDestruction);
        }
        {
            std::stop_source stop;
            auto binding = std::make_unique<Binding>(stop.get_token());
            auto retained = binding->ConsumeToken();
            std::barrier start{2};
            std::jthread requester{[&] {
                start.arrive_and_wait();
                static_cast<void>(stop.request_stop());
            }};
            start.arrive_and_wait();
            binding.reset();
            requester.join();
            REQUIRE(retained.valid());
            if constexpr (CancelOnDestruction) CHECK(retained.cancelled());
            // Without destructor cancellation either race winner is valid; the
            // callback must nevertheless unregister and complete before reset returns.
            CHECK(stop.stop_requested());
        }
    };
    exercise.operator()<false>();
    exercise.operator()<true>();
}
}  // namespace
