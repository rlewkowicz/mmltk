#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/presentation/tests/support/visual_system_fixture.h"
namespace mmltk::controller::visual_test_support {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
class TestLiveAlgorithm final : public LiveAlgorithm {
   public:
    explicit TestLiveAlgorithm(std::shared_ptr<std::atomic<std::uint64_t>> captures, std::shared_ptr<std::atomic_bool> token_changed = {})
        : captures_(std::move(captures)), token_changed_(std::move(token_changed)) {}
    void Start(const LiveStart&) override {}
    void SetOutputAvailableSink(std::function<void()>) override {}
    bool AcquireOutput() override { return true; }
    bool Capture(const mmltk::frameworks::gpu::ImagePlaneView target, std::uintptr_t, const std::stop_token stop) override {
        if (!first_stop_)
            first_stop_ = stop;
        else if (*first_stop_ != stop && token_changed_)
            token_changed_->store(true, std::memory_order_release);
        if (stop.stop_requested()) return false;
        const auto value = captures_->fetch_add(1U, std::memory_order_acq_rel) + 1U;
        Fill(target, static_cast<std::uint8_t>(value));
        return true;
    }
    void Stop() noexcept override {}

   private:
    std::shared_ptr<std::atomic<std::uint64_t>> captures_;
    std::shared_ptr<std::atomic_bool> token_changed_;
    std::optional<std::stop_token> first_stop_;
};
[[nodiscard]] inline VisualRuntimeFactory test_live_runtime_factory(std::shared_ptr<FakeImageBackend> backend,
                                                                    std::shared_ptr<std::atomic<std::uint64_t>> captures,
                                                                    std::shared_ptr<std::atomic_bool> token_changed = {},
                                                                    const std::size_t output_buffer_count = 2U) {
    return RuntimeFactory(
        0, std::move(backend), mmltk::frameworks::gpu::ImageProductLayout::Clean,
        [captures = std::move(captures), token_changed = std::move(token_changed)] { return std::make_unique<TestLiveAlgorithm>(captures, token_changed); },
        output_buffer_count);
}
}  // namespace mmltk::controller::visual_test_support
