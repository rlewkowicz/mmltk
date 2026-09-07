#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "src/common/system/numa_topology.h"
#include "src/common/system/execution_policy.h"

namespace mmltk::common::concurrency {

class WorkerPool final {
   public:
    WorkerPool(size_t worker_count, std::vector<int> cpu_affinity = {}, std::string thread_name_prefix = "fastworker",
               std::size_t queued_capacity = 0U, const mmltk::common::system::ExecutionPlacement* placement = nullptr,
               bool storage_worker = true);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    [[nodiscard]] size_t size() const noexcept;
    // Stable index within this pool; callable only by one of its workers.
    [[nodiscard]] std::size_t current_worker_index() const;
    [[nodiscard]] const mmltk::common::system::ExecutionPolicySnapshot& policy(std::size_t worker) const;
    [[nodiscard]] bool idle() const;
    void wait_idle();
    // Detached work does not allocate an unused future. An uncaught task
    // exception closes admission and is rethrown by wait_idle; result-bearing
    // task exceptions remain owned by the returned future.
    void enqueue_detached(std::function<void()> task);
    // Borrowed records have no type-erasure allocation; owner outlives wait_idle/shutdown.
    void enqueue_borrowed(void* context, std::size_t index, void (*call)(void*, std::size_t));

    template <typename Func>
    auto enqueue(Func&& func) -> std::future<typename std::invoke_result_t<Func>>;

    template <typename Index, typename Func>
    void parallel_for(Index begin, Index end, int max_workers, Func&& func);

   private:
    void shutdown() noexcept;
    [[nodiscard]] bool running_on_worker_thread() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

template <typename Func>
auto WorkerPool::enqueue(Func&& func) -> std::future<typename std::invoke_result_t<Func>> {
    using Result = typename std::invoke_result_t<Func>;
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Func>(func));
    std::future<Result> future = task->get_future();
    enqueue_detached([task]() { (*task)(); });
    return future;
}

template <typename Index, typename Func>
void WorkerPool::parallel_for(Index begin, Index end, int max_workers, Func&& func) {
    static_assert(std::is_integral_v<Index>, "parallel_for index type must be integral");
    if (begin >= end) { return; }
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
    const int worker_count = std::max(1, std::min<int>(std::min<int>(requested_workers, pool_workers), static_cast<int>(total)));
    const Index chunk = (total + static_cast<Index>(worker_count) - 1) / static_cast<Index>(worker_count);
    std::vector<std::future<void>> futures;
    futures.reserve(static_cast<size_t>(worker_count));
    std::exception_ptr failure;
    try {
        for (int worker = 0; worker < worker_count; ++worker) {
            const Index chunk_begin = begin + static_cast<Index>(worker) * chunk;
            if (chunk_begin >= end) break;
            const Index chunk_end = std::min(end, chunk_begin + chunk);
            futures.push_back(enqueue([&, chunk_begin, chunk_end] { func(chunk_begin, chunk_end); }));
        }
    } catch (...) { failure = std::current_exception(); }
    // All borrowed captures finish before an enqueue or task exception escapes.
    for (auto& future : futures) {
        try {
            future.get();
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}

}  // namespace mmltk::common::concurrency
