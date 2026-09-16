#pragma once
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include "src/common/system/execution_policy.h"
#include <semaphore>
#include <stop_token>
#include <thread>
#include <utility>
#include "src/controller/contracts/application_boundary.h"
namespace mmltk::controller::direct {
class LocalRun final {
   public:
    using Notification = std::move_only_function<void()>;
    using Prepare = std::move_only_function<void()>;
    using Work = std::move_only_function<Notification(std::stop_token)>;
    using Failure = std::move_only_function<Notification(std::exception_ptr)>;
    struct Job final {
        std::optional<mmltk::common::system::ExecutionPolicyRequest> policy{};
        Prepare prepare{};
        Work work{};
        Failure failure{};
    };
    LocalRun() = default;
    ~LocalRun() noexcept;
    LocalRun(const LocalRun&) = delete;
    LocalRun& operator=(const LocalRun&) = delete;
    void Start(Job);
    [[nodiscard]] std::stop_source CurrentStopSource() const noexcept;
    [[nodiscard]] bool Stop() noexcept;
    [[nodiscard]] bool active() const noexcept;
    void StopAndJoin() noexcept;

   private:
    struct RunControl final {
        std::binary_semaphore initialized{0};
        std::exception_ptr startup_failure;
        std::binary_semaphore ready{0};
        std::atomic_bool launch = false;
        std::stop_source stop{std::nostopstate};
    };
    void Finish(const std::shared_ptr<RunControl>&) noexcept;
    std::atomic<std::shared_ptr<RunControl>> current_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    bool active_ = false;
};
template <class Sink, class Factory>
void PublishLazyNoexcept(const Sink& sink, Factory factory) noexcept {
    if (!sink) return;
    try {
        sink(factory());
    } catch (...) {}
}
}  // namespace mmltk::controller::direct
