#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <stop_token>
#include <thread>

namespace mmltk::frameworks::gpu {

class SystemImageWorker final {
   public:
    using Cycle = std::function<void(std::stop_token)>;
    using FailureSink = std::function<void(std::exception_ptr)>;
    using Cleanup = std::function<void()>;

    SystemImageWorker(Cycle, FailureSink, Cleanup = {});
    ~SystemImageWorker();
    SystemImageWorker(const SystemImageWorker&) = delete;
    SystemImageWorker& operator=(const SystemImageWorker&) = delete;

    void Wake() noexcept;
    void RequestStop() noexcept;
    void WaitStopped() noexcept;
    [[nodiscard]] bool stopped() const noexcept;

   private:
    void Run(std::stop_token);
    Cycle cycle_;
    FailureSink failures_;
    Cleanup cleanup_;
    static constexpr std::uint8_t kPending = 1U;
    static constexpr std::uint8_t kStopping = 2U;
    std::atomic<std::uint8_t> signals_{0U};
    std::atomic_bool stopped_{false};
    std::jthread worker_;
};

}  // namespace mmltk::frameworks::gpu
