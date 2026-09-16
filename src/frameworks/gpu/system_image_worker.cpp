#include "src/frameworks/gpu/system_image_worker.h"
#include <stdexcept>
#include <utility>
namespace mmltk::frameworks::gpu {
SystemImageWorker::SystemImageWorker(Cycle cycle, FailureSink failures, Cleanup cleanup)
    : cycle_(cycle ? std::move(cycle) : throw std::invalid_argument("system image worker cycle is unavailable")),
      failures_(std::move(failures)),
      cleanup_(std::move(cleanup)),
      worker_([this](const std::stop_token stop) { Run(stop); }) {}
SystemImageWorker::~SystemImageWorker() { RequestStop(); }
void SystemImageWorker::Wake() noexcept {
    {
        std::scoped_lock lock(wait_mutex_);
        signals_.fetch_or(kPending, std::memory_order_release);
    }
    wait_ready_.notify_one();
}
void SystemImageWorker::WakeAt(const std::chrono::steady_clock::time_point deadline) noexcept {
    {
        std::scoped_lock lock(wait_mutex_);
        if (!deadline_ || deadline < *deadline_) deadline_ = deadline;
    }
    wait_ready_.notify_one();
}
void SystemImageWorker::RequestStop() noexcept {
    {
        std::scoped_lock lock(wait_mutex_);
        if ((signals_.fetch_or(kStopping, std::memory_order_acq_rel) & kStopping) != 0U) return;
    }
    worker_.request_stop();
    wait_ready_.notify_all();
}
void SystemImageWorker::WaitStopped() noexcept { stopped_.wait(false, std::memory_order_acquire); }
bool SystemImageWorker::stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }
void SystemImageWorker::Run(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(wait_mutex_);
            while (signals_.load(std::memory_order_acquire) == 0U) {
                if (deadline_) {
                    if (std::chrono::steady_clock::now() >= *deadline_) {
                        deadline_.reset();
                        signals_.fetch_or(kPending, std::memory_order_release);
                        break;
                    }
                    wait_ready_.wait_until(lock, *deadline_);
                } else {
                    wait_ready_.wait(lock);
                }
            }
        }
        const auto signals = signals_.fetch_and(static_cast<std::uint8_t>(~kPending), std::memory_order_acq_rel);
        if ((signals & kStopping) != 0U || stop.stop_requested()) break;
        try {
            cycle_(stop);
        } catch (...) {
            if (failures_) {
                try {
                    failures_(std::current_exception());
                } catch (...) { failures_ = {}; }
            }
        }
    }
    if (cleanup_) {
        try {
            cleanup_();
        } catch (...) {
            if (failures_) {
                try {
                    failures_(std::current_exception());
                } catch (...) {}
            }
        }
    }
    stopped_.store(true, std::memory_order_release);
    stopped_.notify_all();
}
}  // namespace mmltk::frameworks::gpu
