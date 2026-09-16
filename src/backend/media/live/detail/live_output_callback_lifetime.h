#pragma once
#include <atomic>
#include <cstddef>
namespace mmltk::backend::media::live {
class LiveOutputCallbackLifetime final {
   public:
    using Wake = void (*)(void*) noexcept;
    LiveOutputCallbackLifetime(const std::size_t capacity, void* owner, const Wake wake) noexcept : capacity_(capacity), owner_(owner), wake_(wake) {}
    [[nodiscard]] bool acquire() noexcept {
        std::size_t active = active_.load(std::memory_order_relaxed);
        while (active < capacity_) {
            if (active_.compare_exchange_weak(active, active + 1U, std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
        }
        return false;
    }
    void release() noexcept {
        std::size_t active = active_.load(std::memory_order_relaxed);
        for (;;) {
            if (active == 0U) return;
            if (active_.compare_exchange_weak(active, active - 1U, std::memory_order_acq_rel, std::memory_order_relaxed)) break;
        }
        if (wake_ == nullptr) return;
        wake_(owner_);
    }
    [[nodiscard]] bool idle() const noexcept { return active_.load(std::memory_order_acquire) == 0U; }

   private:
    std::size_t capacity_ = 0U;
    void* owner_ = nullptr;
    Wake wake_ = nullptr;
    std::atomic<std::size_t> active_{0U};
};
class LiveOutputCallbackGuard final {
   public:
    explicit LiveOutputCallbackGuard(LiveOutputCallbackLifetime& lifetime) noexcept : lifetime_(&lifetime) {}
    ~LiveOutputCallbackGuard() { lifetime_->release(); }
    LiveOutputCallbackGuard(const LiveOutputCallbackGuard&) = delete;
    LiveOutputCallbackGuard& operator=(const LiveOutputCallbackGuard&) = delete;

   private:
    LiveOutputCallbackLifetime* lifetime_;
};
}  // namespace mmltk::backend::media::live
