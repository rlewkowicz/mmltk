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
    signals_.fetch_or(kPending, std::memory_order_release);
    signals_.notify_one();
}

void SystemImageWorker::RequestStop() noexcept {
    if ((signals_.fetch_or(kStopping, std::memory_order_acq_rel) & kStopping) != 0U) return;
    worker_.request_stop();
    signals_.notify_all();
}

void SystemImageWorker::WaitStopped() noexcept {
    stopped_.wait(false, std::memory_order_acquire);
}

bool SystemImageWorker::stopped() const noexcept {
    return stopped_.load(std::memory_order_acquire);
}

void SystemImageWorker::Run(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        signals_.wait(0U, std::memory_order_acquire);
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
