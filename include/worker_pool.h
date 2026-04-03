#pragma once

#include "cpu_affinity.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mmltk {

class WorkerPool {
public:
    WorkerPool(size_t worker_count,
               std::vector<int> cpu_affinity = {},
               std::string thread_name_prefix = "fastworker");
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    [[nodiscard]] size_t size() const noexcept;
    void wait_idle();

    template <typename Func>
    auto enqueue(Func&& func) -> std::future<typename std::invoke_result_t<Func>>;

    template <typename Index, typename Func>
    void parallel_for(Index begin, Index end, int max_workers, Func&& func);

private:
    void enqueue_task(std::function<void()> task);
    [[nodiscard]] bool running_on_worker_thread() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

template <typename Func>
auto WorkerPool::enqueue(Func&& func) -> std::future<typename std::invoke_result_t<Func>> {
    using Result = typename std::invoke_result_t<Func>;
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Func>(func));
    std::future<Result> future = task->get_future();
    enqueue_task([task]() { (*task)(); });
    return future;
}

template <typename Index, typename Func>
void WorkerPool::parallel_for(Index begin, Index end, int max_workers, Func&& func) {
    static_assert(std::is_integral_v<Index>, "parallel_for index type must be integral");
    if (begin >= end) {
        return;
    }
    if (running_on_worker_thread()) {
        func(begin, end);
        return;
    }

    const Index total = end - begin;
    const int pool_workers = static_cast<int>(size());
    if (pool_workers <= 0) {
        func(begin, end);
        return;
    }
    const int requested_workers = max_workers > 0 ? max_workers : pool_workers;
    const int worker_count = std::max(
        1,
        std::min<int>(std::min<int>(requested_workers, pool_workers), static_cast<int>(total)));
    if (worker_count == 1) {
        func(begin, end);
        return;
    }

    const Index chunk = (total + static_cast<Index>(worker_count) - 1) / static_cast<Index>(worker_count);
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<size_t>(std::max(worker_count - 1, 0)));
    for (int worker = 0; worker < worker_count; ++worker) {
        const Index chunk_begin = begin + static_cast<Index>(worker) * chunk;
        if (chunk_begin >= end) {
            break;
        }
        const Index chunk_end = std::min(end, chunk_begin + chunk);
        if (worker == worker_count - 1) {
            func(chunk_begin, chunk_end);
        } else {
            futures.push_back(enqueue([&, chunk_begin, chunk_end] {
                func(chunk_begin, chunk_end);
            }));
        }
    }
    for (auto& future : futures) {
        future.get();
    }
}

} // namespace mmltk
