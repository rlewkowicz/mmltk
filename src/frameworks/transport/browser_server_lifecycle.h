#pragma once

#include <cstdint>

namespace mmltk::frameworks::transport::detail {

struct BrowserPeerLifecycle final {
    std::uint64_t generation = 0U;
    bool closure_notified = true;

    void opened(const std::uint64_t value) noexcept {
        generation = value;
        closure_notified = false;
    }

    [[nodiscard]] bool begin_close() noexcept {
        if (closure_notified) return false;
        closure_notified = true;
        return true;
    }
};

enum class BrowserOutputPhase : std::uint8_t {
    Closed,
    Opening,
    Open,
};

struct BrowserOutputEpoch final {
    std::uint64_t generation = 0U;
    BrowserOutputPhase phase = BrowserOutputPhase::Closed;

    void begin_open(const std::uint64_t value) noexcept {
        generation = value;
        phase = BrowserOutputPhase::Opening;
    }

    void finish_open(const std::uint64_t value) noexcept {
        if (generation == value && phase == BrowserOutputPhase::Opening) phase = BrowserOutputPhase::Open;
    }

    void close() noexcept { phase = BrowserOutputPhase::Closed; }

    [[nodiscard]] bool admits(const bool owner_thread) const noexcept {
        return phase == BrowserOutputPhase::Open || (phase == BrowserOutputPhase::Opening && owner_thread);
    }
};

}  // namespace mmltk::frameworks::transport::detail
