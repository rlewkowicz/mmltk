#pragma once
#include <atomic>
#include <utility>
namespace mmltk::common::concurrency {
class ExclusiveOperationGate final {
   public:
    class Guard final {
       public:
        Guard() noexcept = default;
        explicit Guard(ExclusiveOperationGate* gate) noexcept : gate_(gate) {}
        ~Guard() { release(); }
        // CLEANUP-IGNORE: This move-only synchronization guard has semantics unrelated to CBOR encoder storage.
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                release();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        explicit operator bool() const noexcept { return gate_ != nullptr; }

       private:
        void release() noexcept {
            if (gate_ != nullptr) {
                gate_->held_.clear(std::memory_order_release);
                gate_ = nullptr;
            }
        }
        ExclusiveOperationGate* gate_ = nullptr;
    };
    [[nodiscard]] Guard try_acquire() noexcept { return Guard{held_.test_and_set(std::memory_order_acquire) ? nullptr : this}; }
    [[nodiscard]] bool in_flight() const noexcept { return held_.test(std::memory_order_acquire); }

   private:
    std::atomic_flag held_ = ATOMIC_FLAG_INIT;
};
}  // namespace mmltk::common::concurrency
