#pragma once  // backend.data private implementation boundary
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
template <class Task, class BeforeUnlock>
[[nodiscard]] std::optional<Task> wait_pop_task(std::mutex& mutex, std::condition_variable& pending, bool& stopping, std::deque<Task>& tasks,
                                                const mmltk::common::concurrency::CancellationObservation cancellation, BeforeUnlock&& before_unlock) {
    std::unique_lock lock(mutex);
    pending.wait(lock, [&] { return stopping || cancellation.requested() || !tasks.empty(); });
    if (cancellation.requested()) {
        stopping = true;
        tasks.clear();
    }
    if (stopping && tasks.empty()) { return std::nullopt; }
    Task task = std::move(tasks.front());
    tasks.pop_front();
    std::forward<BeforeUnlock>(before_unlock)();
    return task;
}
template <class Task>
[[nodiscard]] std::optional<Task> wait_pop_task(std::mutex& mutex, std::condition_variable& pending, bool& stopping, std::deque<Task>& tasks,
                                                const mmltk::common::concurrency::CancellationObservation cancellation) {
    return wait_pop_task(mutex, pending, stopping, tasks, cancellation, [] {});
}
}  // namespace mmltk::backend::data::benchmark_internal
