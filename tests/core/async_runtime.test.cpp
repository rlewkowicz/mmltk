#include "mmltk/runtime/async_runtime.h"

#include <atomic>
#include "support/catch2_compat.hpp"
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace mmltk::runtime;

void drain_until(UiCallbackQueue& queue, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done()) {
        queue.drain();
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    queue.drain();
}

void test_success_callback_runs_on_ui_drain() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    std::atomic<bool> success_called{false};

    (void)submit_background_task(
        executor,
        queue,
        []() { return 7; },
        [&](int value) {
            assert(value == 7);
            success_called.store(true);
        },
        [&](const std::string&) {
            throw std::runtime_error("unexpected error callback");
        });

    drain_until(queue, [&]() { return success_called.load(); });
    executor.wait_idle();
    assert(success_called.load());
}

void test_error_callback_propagates_message() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    std::atomic<bool> error_called{false};

    (void)submit_background_task(
        executor,
        queue,
        []() -> int {
            throw std::runtime_error("boom");
        },
        [&](int) {
            throw std::runtime_error("unexpected success callback");
        },
        [&](const std::string& message) {
            assert(message == "boom");
            error_called.store(true);
        });

    drain_until(queue, [&]() { return error_called.load(); });
    executor.wait_idle();
    assert(error_called.load());
}

void test_cancellation_drops_callbacks() {
    BackgroundExecutor executor(1);
    UiCallbackQueue queue;
    std::atomic<bool> callback_called{false};

    const TaskCancellation token = submit_background_task(
        executor,
        queue,
        []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return 11;
        },
        [&](int) { callback_called.store(true); },
        [&](const std::string&) { callback_called.store(true); });
    token.cancel();

    executor.wait_idle();
    queue.drain();
    assert(!callback_called.load());
}

} // namespace

MMLTK_REGISTER_TEST_CASE("[core][async_runtime]", test_success_callback_runs_on_ui_drain);
MMLTK_REGISTER_TEST_CASE("[core][async_runtime]", test_error_callback_propagates_message);
MMLTK_REGISTER_TEST_CASE("[core][async_runtime]", test_cancellation_drops_callbacks);
