#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/common/io/scoped_fd.h"
#include "src/test_support/async_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sys/socket.h>
#include <poll.h>
#include <array>
#include <chrono>
#include <future>
#include <thread>
namespace mmltk::controller {
namespace {
class AcceptanceGateFixture final {
   public:
    explicit AcceptanceGateFixture(const int transport) : AcceptanceGateFixture(OpenSockets(transport)) {}
    [[nodiscard]] ExploreAcceptanceGate& gate() noexcept { return gate_; }
    [[nodiscard]] mmltk::common::io::ScopedFd& commands() noexcept { return commands_; }
    template <class T>
    T Await(std::future<T>& result) {
        const auto status = result.wait_for(std::chrono::seconds{2});
        if (status != std::future_status::ready) gate_.Stop();
        REQUIRE(status == std::future_status::ready);
        return result.get();
    }

   private:
    using Sockets = std::array<mmltk::common::io::ScopedFd, 2U>;
    [[nodiscard]] static Sockets OpenSockets(const int transport) {
        std::array<int, 2U> sockets{};
        REQUIRE(::socketpair(AF_UNIX, transport | SOCK_CLOEXEC, 0, sockets.data()) == 0);
        return {mmltk::common::io::ScopedFd{sockets[0]}, mmltk::common::io::ScopedFd{sockets[1]}};
    }
    explicit AcceptanceGateFixture(Sockets sockets) : commands_(std::move(sockets[0])), gate_(sockets[1].release()) {}
    mmltk::common::io::ScopedFd commands_;
    ExploreAcceptanceGate gate_;
};
TEST_CASE("acceptance gate retains one reader across settled frontend workflows", "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    std::promise<contracts::IntegrationControlReceipt> advanced;
    auto delivered = advanced.get_future();
    AcceptanceGateFixture fixture{transport};
    auto& commands = fixture.commands();
    auto& gate = fixture.gate();
    gate.SetFrontendCommand([&advanced](const auto receipt) {
        advanced.set_value(receipt);
        return true;
    });
    const std::uint8_t release_held = 4U;
    REQUIRE(::send(commands.get(), &release_held, sizeof(release_held), MSG_NOSIGNAL) == sizeof(release_held));
    REQUIRE(gate.ClaimHeldCompletion());
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(0U, 4U, 8U, 64U); });
    CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Proceed);
    CHECK_FALSE(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 2U}));
    for (const auto kind : {Kind::Progress, Kind::Settled, Kind::PressureEntered})
        CHECK_FALSE(gate.ObserveFrontend({.kind = kind, .sequence = 1U, .progress = 1U, .failureline = 123U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Advance, .sequence = 1U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    const std::uint8_t advance = 8U;
    REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    CHECK((fixture.Await(delivered) == contracts::IntegrationControlReceipt{.kind = Kind::Advance, .sequence = 2U}));
    CHECK(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 1U}));
    CHECK(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 2U, .progress = 1U}));
    gate.SetFrontendCommand({});
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Settled, .sequence = 2U}));
    gate.Stop();
    CHECK(gate.ClaimTerminalReport());
    CHECK_FALSE(gate.ClaimTerminalReport());
}
TEST_CASE("acceptance gate drains queued worker commands and wakes stale waits", "[explore][acceptance][control]") {
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    AcceptanceGateFixture fixture{transport};
    auto& commands = fixture.commands();
    auto& gate = fixture.gate();
    for (const auto command : std::array<std::uint8_t, 3U>{1U, 2U, 4U})
        REQUIRE(::send(commands.get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    gate.AdvanceGeneration(7U);
    auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
    CHECK(fixture.Await(initial) == ExploreAcceptanceGate::WaitResult::Proceed);
    REQUIRE(gate.ClaimHeldCompletion());
    gate.AdvanceGeneration(7U);
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 4U, 8U, 64U); });
    CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Proceed);
    CHECK_FALSE(gate.ClaimHeldCompletion());
    gate.AdvanceGeneration(8U);
    CHECK(gate.AwaitInitialRelease(7U) == ExploreAcceptanceGate::WaitResult::Stale);
    gate.Stop();
    CHECK(gate.AwaitInitialRelease(8U) == ExploreAcceptanceGate::WaitResult::Stale);
}
TEST_CASE("acceptance failure forwards exact bounded text in its terminal packet", "[explore][acceptance][control]") {
    const auto size = GENERATE(std::size_t{0U}, std::size_t{17U}, contracts::kIntegrationFailureMaxBytes);
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& gate = fixture.gate();
    gate.SetFrontendCommand([](auto) { return true; });
    contracts::IntegrationControlReceipt receipt{
        .kind = contracts::IntegrationControlKind::Failed, .sequence = 1U, .progress = 87U, .failureline = 123U, .failure = std::string(size, 'x')};
    auto invalid = receipt;
    invalid.sequence = 2U;
    CHECK_FALSE(gate.ObserveFrontend(invalid));
    invalid = receipt;
    invalid.failure.assign(contracts::kIntegrationFailureMaxBytes + 1U, 'x');
    CHECK_FALSE(gate.ObserveFrontend(invalid));
    REQUIRE(gate.ObserveFrontend(receipt));
    std::array<char, sizeof(ExploreAcceptanceGate::ControlObservation) + contracts::kIntegrationFailureMaxBytes> packet;
    const auto received = ::recv(fixture.commands().get(), packet.data(), packet.size(), MSG_DONTWAIT | MSG_TRUNC);
    REQUIRE(received == static_cast<ssize_t>(sizeof(ExploreAcceptanceGate::ControlObservation) + size));
    ExploreAcceptanceGate::ControlObservation header;
    std::memcpy(&header, packet.data(), sizeof(header));
    CHECK(header.event == ExploreAcceptanceGate::ControlEvent::Frontend);
    CHECK(header.generation == receipt.sequence);
    CHECK(header.slot == static_cast<std::uint64_t>(receipt.kind));
    CHECK(header.compiled_index == receipt.progress);
    CHECK(header.staging_bytes == receipt.failureline);
    CHECK(std::string_view(packet.data() + sizeof(header), size) == receipt.failure);
    CHECK(gate.ClaimTerminalReport());
    CHECK_FALSE(gate.ObserveFrontend(receipt));
}
TEST_CASE("acceptance gate rejects premature advancement and unavailable callbacks", "[explore][acceptance][control]") {
    const unsigned terminal_path = GENERATE(0U, 1U, 2U, 3U, 4U);
    std::promise<void> invoked;
    auto invocation = invoked.get_future();
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& commands = fixture.commands();
    auto& gate = fixture.gate();
    gate.SetFrontendCommand([terminal_path, &invoked](auto) {
        invoked.set_value();
        return terminal_path == 4U;
    });
    if (terminal_path == 1U || terminal_path == 4U) REQUIRE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Settled, .sequence = 1U}));
    if (terminal_path == 2U) {
        CHECK_FALSE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Failed, .sequence = 1U}));
        REQUIRE(gate.ObserveFrontend({.kind = contracts::IntegrationControlKind::Failed, .sequence = 1U, .failureline = 123U}));
        ExploreAcceptanceGate::ControlObservation observation{};
        REQUIRE(::recv(commands.get(), &observation, sizeof(observation), MSG_DONTWAIT) == sizeof(observation));
        CHECK(observation.event == ExploreAcceptanceGate::ControlEvent::Frontend);
        CHECK(observation.generation == 1U);
        CHECK(observation.compiled_index == 0U);
        CHECK(observation.staging_bytes == 123U);
    }
    gate.AdvanceGeneration(1U);
    const std::uint8_t advance = 8U;
    if (terminal_path < 2U || terminal_path == 4U) REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    if (terminal_path == 1U || terminal_path == 4U) fixture.Await(invocation);
    if (terminal_path == 4U) REQUIRE(::send(commands.get(), &advance, sizeof(advance), MSG_NOSIGNAL) == sizeof(advance));
    if (terminal_path == 3U) commands.reset();
    auto waiting = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(1U); });
    CHECK(fixture.Await(waiting) == ExploreAcceptanceGate::WaitResult::Stale);
    CHECK(gate.ClaimTerminalReport());
}
TEST_CASE("acceptance redraw is one shot and preserves the held read boundary", "[explore][acceptance][control]") {
    const int transport = GENERATE(SOCK_STREAM, SOCK_SEQPACKET);
    // Successful redraw, callback refusal, callback exception, unavailable
    // callback, duplicate command, and redraw after all/one read was released.
    const unsigned path = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U);
    std::promise<void> invoked;
    auto invocation = invoked.get_future();
    AcceptanceGateFixture fixture{transport};
    auto& gate = fixture.gate();
    const auto send = [&](const std::uint8_t command) {
        REQUIRE(::send(fixture.commands().get(), &command, sizeof(command), MSG_NOSIGNAL) == sizeof(command));
    };
    gate.AdvanceGeneration(7U);
    if (path != 3U) {
        gate.SetRedrawCommand([&] {
            // This takes the gate mutex: callbacks must execute outside it.
            gate.AdvanceGeneration(7U);
            invoked.set_value();
            if (path == 2U) throw std::runtime_error("acceptance redraw failure");
            return path != 1U;
        });
    }
    if (path == 5U) send(2U);
    if (path == 6U) {
        send(1U);
        auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
        CHECK(fixture.Await(initial) == ExploreAcceptanceGate::WaitResult::Proceed);
    }
    send(16U);
    if (path < 3U || path == 4U) fixture.Await(invocation);
    if (path == 0U) {
        auto initial = std::async(std::launch::async, [&] { return gate.AwaitInitialRelease(7U); });
        mmltk::testsupport::ScopedTestCleanup cleanup{[&] { gate.Stop(); }};
        // The real initial-wait receipt proves redraw left reads held.
        pollfd waiting{.fd = fixture.commands().get(), .events = POLLIN, .revents = 0};
        int ready = -1;
        do { ready = ::poll(&waiting, 1U, 2000); } while (ready < 0 && errno == EINTR);
        if (ready != 1) gate.Stop();
        REQUIRE(ready == 1);
        std::uint8_t observation = 0U;
        REQUIRE(::recv(fixture.commands().get(), &observation, sizeof(observation), MSG_DONTWAIT) == sizeof(observation));
        CHECK(observation == static_cast<std::uint8_t>(ExploreAcceptanceGate::ControlEvent::InitialWait));
        CHECK(initial.wait_for(std::chrono::seconds{0}) != std::future_status::ready);
        send(1U);
        CHECK(fixture.Await(initial) == ExploreAcceptanceGate::WaitResult::Proceed);
        gate.StopAndJoin();
    } else {
        if (path == 4U) send(16U);
        // This wait is independent of release-all, including path 5. Only
        // rejection or shutdown can settle it without command 4.
        auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 0U, 0U, 64U); });
        CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Stale);
        gate.StopAndJoin();
    }
    CHECK(gate.ClaimTerminalReport());
    CHECK(gate.AwaitInitialRelease(7U) == ExploreAcceptanceGate::WaitResult::Stale);
}
}  // namespace
}  // namespace mmltk::controller
namespace mmltk::controller {
namespace {
TEST_CASE("completed gallery read receipts retain identity through reentrant supersession and terminal callbacks", "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    const auto outcome = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U);
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& gate = fixture.gate();
    std::optional<contracts::IntegrationControlReceipt> received;
    gate.SetFrontendCommand([&](const auto receipt) {
        CHECK(gate.FrontendSequence() == 1U);
        REQUIRE_FALSE(received.has_value());
        received = receipt;
        if (outcome == 0U) gate.AdvanceGeneration(8U);
        if (outcome == 4U || outcome == 5U) {
            gate.AdvanceGeneration(outcome == 4U ? 8U : 0U);
            gate.AdvanceGeneration(7U);
        }
        if (outcome == 2U) throw std::runtime_error("fixture callback failed");
        if (outcome == 3U) gate.Stop();
        return outcome != 1U;
    });
    gate.AdvanceGeneration(7U);
    REQUIRE(gate.ClaimHeldCompletion());
    CHECK_FALSE(gate.ClaimHeldCompletion());
    if (outcome == 6U || outcome == 7U) {
        gate.AdvanceGeneration(outcome == 6U ? 8U : 0U);
        gate.AdvanceGeneration(7U);
    }
    auto held = std::async(std::launch::async, [&] { return gate.AwaitHeldCompletion(7U, 3U, 47U, 1024U); });
    CHECK(fixture.Await(held) == ExploreAcceptanceGate::WaitResult::Stale);
    REQUIRE(received.has_value());
    CHECK((*received ==
           contracts::IntegrationControlReceipt{.kind = Kind::GalleryReadCompletionHeld, .sequence = 1U, .read_generation = 7U, .compiled_index = 47U}));
    for (const auto expected : {ExploreAcceptanceGate::ControlEvent::HeldWait, ExploreAcceptanceGate::ControlEvent::HeldStale}) {
        ExploreAcceptanceGate::ControlObservation observation{};
        REQUIRE(::recv(fixture.commands().get(), &observation, sizeof(observation), MSG_DONTWAIT) == sizeof(observation));
        CHECK(observation.event == expected);
        CHECK(observation.generation == 7U);
        CHECK(observation.slot == 3U);
        CHECK(observation.compiled_index == 47U);
        CHECK(observation.staging_bytes == 1024U);
    }
    CHECK(gate.ClaimTerminalReport() == (outcome >= 1U && outcome <= 3U));
    gate.StopAndJoin();
}
TEST_CASE("visible read gate holds the exact image across demand changes and rejects duplicate receipts", "[explore][acceptance][control]") {
    using Kind = contracts::IntegrationControlKind;
    AcceptanceGateFixture fixture{SOCK_SEQPACKET};
    auto& gate = fixture.gate();
    std::promise<void> armed, observed;
    auto arm = armed.get_future(), held = observed.get_future();
    gate.SetFrontendCommand([&](const auto receipt) {
        // This reentrant owner access proves callbacks run outside the gate mutex.
        CHECK(gate.FrontendSequence() == 1U);
        if (receipt.kind == Kind::VisibleReadArmed) armed.set_value();
        if (receipt.kind == Kind::VisibleReadHeld) {
            CHECK(receipt.read_generation == 7U);
            CHECK(receipt.compiled_index == 0U);
            observed.set_value();
        }
        return true;
    });
    gate.AdvanceGeneration(7U);
    REQUIRE(gate.ObserveFrontend({.kind = Kind::VisibleReadArmRequested, .sequence = 1U, .compiled_index = 0U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::VisibleReadArmRequested, .sequence = 1U}));
    const auto send = [&](const ExploreAcceptanceGate::ControlCommand command) {
        const auto value = static_cast<std::uint8_t>(command);
        REQUIRE(::send(fixture.commands().get(), &value, sizeof(value), MSG_NOSIGNAL) == sizeof(value));
    };
    send(ExploreAcceptanceGate::ControlCommand::ArmVisibleRead);
    fixture.Await(arm);
    gate.AwaitVisibleRead(7U, 1U);  // Another compiled descriptor is not held.
    auto reading = std::async(std::launch::async, [&] { gate.AwaitVisibleRead(7U, 0U); });
    fixture.Await(held);
    gate.AdvanceGeneration(8U);
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::VisibleReadReleaseRequested, .sequence = 1U, .read_generation = 8U}));
    REQUIRE(gate.ObserveFrontend({.kind = Kind::VisibleReadReleaseRequested, .sequence = 1U, .read_generation = 7U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::VisibleReadReleaseRequested, .sequence = 1U, .read_generation = 7U}));
    send(ExploreAcceptanceGate::ControlCommand::ReleaseVisibleRead);
    fixture.Await(reading);
    CHECK(gate.ObserveFrontend({.kind = Kind::CapacityArmRequested, .sequence = 1U}));
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::CapacityArmRequested, .sequence = 1U}));
    gate.StopAndJoin();
    CHECK_FALSE(gate.ObserveFrontend({.kind = Kind::Progress, .sequence = 1U, .progress = 1U}));
}
TEST_CASE("native completion gate preserves the capacity-before-consumption wake", "[presentation][acceptance][control]") {
    PresentationAcceptanceGate gate;
    std::vector<PresentationAcceptanceGate::Receipt> receipts;
    unsigned wakes = 0U;
    gate.SetWake([&] {
        ++wakes;
        gate.SetWake({});
    });
    gate.SetObserver([&](const auto receipt) {
        receipts.push_back(receipt);
        gate.SetObserver({});
    });
    CHECK_FALSE(gate.Release());
    REQUIRE(gate.Arm());
    CHECK_FALSE(gate.Arm());
    const PresentationAcceptanceGate::Receipt held{3U, 4U, 5U, 6U};
    REQUIRE(gate.Hold(held));
    REQUIRE(receipts.size() == 1U);
    CHECK(receipts.front().boundary == PresentationAcceptanceGate::Boundary::Completion);
    gate.SetObserver([&](const auto receipt) { receipts.push_back(receipt); });
    gate.ObserveCapacity();
    gate.ObserveCapacity();
    REQUIRE(receipts.size() == 2U);
    CHECK(receipts.back().boundary == PresentationAcceptanceGate::Boundary::Capacity);
    CHECK(receipts.back().publication == held.publication);
    REQUIRE(gate.Release());
    CHECK(wakes == 1U);  // Explicit wake survives an already drained completion edge.
    CHECK_FALSE(gate.Hold(held));
    CHECK_FALSE(gate.Release());
    CHECK_FALSE(gate.ReleaseSupersession());
    gate.SetWake([&] {
        ++wakes;
        CHECK_FALSE(gate.SupersessionHeld());
    });
    gate.HoldSupersession(7U, 8U);
    REQUIRE(gate.SupersessionHeld());
    REQUIRE(receipts.size() == 3U);
    CHECK(receipts.back().boundary == PresentationAcceptanceGate::Boundary::Supersession);
    CHECK(receipts.back().source_high == 7U);
    CHECK(receipts.back().source_low == 8U);
    gate.HoldSupersession(9U, 10U);
    CHECK(receipts.size() == 3U);
    REQUIRE(gate.ReleaseSupersession());
    CHECK(wakes == 2U);
    CHECK_FALSE(gate.ReleaseSupersession());
    gate.HoldSupersession(11U, 12U);
    gate.Stop();
    CHECK_FALSE(gate.SupersessionHeld());
    CHECK_FALSE(gate.Arm());
}
}  // namespace
}  // namespace mmltk::controller
