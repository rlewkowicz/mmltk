#include "src/common/concurrency/worker_pool.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"

namespace mmltk::common::concurrency {

struct WorkerPool::Impl final {
    mutable std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::condition_variable startup_cv;
    struct Task {
        std::function<void()> owned{};
        void* context = nullptr;
        std::size_t index = 0;
        void (*borrowed)(void*, std::size_t) = nullptr;
        void operator()() {
            if (borrowed)
                borrowed(context, index);
            else
                owned();
        }
    };
    std::vector<Task> tasks;
    std::size_t task_head = 0U;
    std::size_t task_count = 0U;

    void push(Task task) {
        if (task_count == tasks.size()) {
            if (tasks.size() > tasks.max_size() / 2U) throw std::length_error("worker task queue capacity exhausted");
            std::vector<Task> grown(std::max<std::size_t>(1U, tasks.size() * 2U));
            for (std::size_t index = 0U; index < task_count; ++index)
                grown[index] = std::move(tasks[(task_head + index) % tasks.size()]);
            tasks = std::move(grown);
            task_head = 0U;
        }
        tasks[(task_head + task_count) % tasks.size()] = std::move(task);
        ++task_count;
    }

    Task pop() noexcept {
        auto task = std::move(tasks[task_head]);
        task_head = (task_head + 1U) % tasks.size();
        --task_count;
        return task;
    }
    std::vector<std::thread> workers;
    std::vector<int> cpu_affinity;
    std::string thread_name_prefix;
    std::vector<int> worker_nodes;
    std::vector<mmltk::common::system::ExecutionPolicySnapshot> policies;
    bool storage_worker = true;
    std::size_t pending_tasks = 0U;
    std::size_t started_workers = 0U;
    bool shutdown = false;
    std::exception_ptr startup_error;
};

namespace {

thread_local const WorkerPool* current_worker_pool = nullptr;
thread_local std::size_t current_pool_index = 0U;

std::string worker_name_for_index(const std::string& prefix, const std::size_t index) {
    if (prefix.empty()) { return {}; }
    std::string name = prefix + std::to_string(index);
    if (name.size() > 15U) { name.resize(15U); }
    return name;
}

}  // namespace

WorkerPool::WorkerPool(const std::size_t worker_count, std::vector<int> cpu_affinity, std::string thread_name_prefix,
                       const std::size_t queued_capacity, const mmltk::common::system::ExecutionPlacement* placement,
                       const bool storage_worker)
    : impl_(std::make_unique<Impl>()) {
    if (worker_count == 0U || worker_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("worker_count must be greater than zero");
    }
    if (placement) {
        if (placement->numa_node < 0 || placement->cpus.empty()) throw std::invalid_argument("worker placement is unresolved");
        for (int cpu : placement->cpus)
            if (cpu_affinity.empty() || std::ranges::find(cpu_affinity, cpu) != cpu_affinity.end()) impl_->cpu_affinity.push_back(cpu);
        if (impl_->cpu_affinity.empty()) throw std::invalid_argument("worker CPU eligibility excludes resolved placement");
        impl_->worker_nodes.assign(impl_->cpu_affinity.size(), placement->numa_node);
    } else {
        const auto topology = mmltk::common::system::NumaTopology::Capture();
        impl_->cpu_affinity =
            mmltk::common::system::physical_core_order(topology, cpu_affinity.empty() ? topology.permitted_cpus : cpu_affinity);
        for (int cpu : impl_->cpu_affinity) {
            if (std::ranges::find(topology.permitted_cpus, cpu) == topology.permitted_cpus.end())
                throw std::invalid_argument("worker CPU is outside permitted cpuset");
            impl_->worker_nodes.push_back(std::ranges::find(topology.cpus, cpu, &mmltk::common::system::CpuTopology::cpu)->node);
        }
    }
    impl_->storage_worker = storage_worker;
    impl_->thread_name_prefix = std::move(thread_name_prefix);
    const auto actual_worker_count = placement ? worker_count
                                               : static_cast<std::size_t>(mmltk::common::system::clamp_worker_count_to_cpus(
                                                     static_cast<int>(worker_count), impl_->cpu_affinity.size(), 0, 1));
    impl_->tasks.resize(std::max(queued_capacity, actual_worker_count));
    impl_->workers.reserve(actual_worker_count);
    impl_->policies.resize(actual_worker_count);
    try {
        for (std::size_t worker_index = 0U; worker_index < actual_worker_count; ++worker_index) {
            impl_->workers.emplace_back([this, worker_index] {
                try {
                    impl_->policies[worker_index] =
                        mmltk::common::system::apply_worker_execution_policy(mmltk::common::system::ExecutionPolicyRequest{
                            impl_->cpu_affinity,
                            worker_name_for_index(impl_->thread_name_prefix, worker_index),
                            worker_index,
                            impl_->worker_nodes[worker_index % impl_->worker_nodes.size()],
                            -10,
                            impl_->storage_worker,
                        });
                } catch (...) {
                    std::lock_guard lock(impl_->mutex);
                    if (impl_->startup_error == nullptr) { impl_->startup_error = std::current_exception(); }
                    impl_->shutdown = true;
                    ++impl_->started_workers;
                    impl_->startup_cv.notify_all();
                    impl_->work_cv.notify_all();
                    impl_->idle_cv.notify_all();
                    return;
                }
                {
                    std::lock_guard lock(impl_->mutex);
                    ++impl_->started_workers;
                }
                impl_->startup_cv.notify_all();
                current_worker_pool = this;
                current_pool_index = worker_index;
                while (true) {
                    Impl::Task task;
                    {
                        std::unique_lock lock(impl_->mutex);
                        impl_->work_cv.wait(lock, [this] { return impl_->shutdown || impl_->task_count != 0U; });
                        if (impl_->shutdown && impl_->task_count == 0U) {
                            current_worker_pool = nullptr;
                            return;
                        }
                        task = impl_->pop();
                    }
                    try {
                        task();
                    } catch (...) {
                        std::lock_guard lock(impl_->mutex);
                        impl_->shutdown = true;
                        if (impl_->startup_error == nullptr) { impl_->startup_error = std::current_exception(); }
                        impl_->work_cv.notify_all();
                        impl_->idle_cv.notify_all();
                        impl_->startup_cv.notify_all();
                    }
                    {
                        std::lock_guard lock(impl_->mutex);
                        if (impl_->pending_tasks == 0U) {
                            impl_->shutdown = true;
                            if (impl_->startup_error == nullptr) {
                                impl_->startup_error =
                                    std::make_exception_ptr(std::runtime_error("worker pool internal task accounting underflow"));
                            }
                            impl_->work_cv.notify_all();
                            impl_->idle_cv.notify_all();
                            impl_->startup_cv.notify_all();
                            current_worker_pool = nullptr;
                            return;
                        }
                        --impl_->pending_tasks;
                        if (impl_->pending_tasks == 0U) { impl_->idle_cv.notify_all(); }
                    }
                }
            });
        }
        std::unique_lock lock(impl_->mutex);
        impl_->startup_cv.wait(
            lock, [this, actual_worker_count] { return impl_->started_workers == actual_worker_count || impl_->startup_error != nullptr; });
        if (impl_->startup_error != nullptr) std::rethrow_exception(impl_->startup_error);
    } catch (...) {
        shutdown();
        throw;
    }
}

WorkerPool::~WorkerPool() { shutdown(); }

void WorkerPool::shutdown() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->shutdown = true;
    }
    impl_->work_cv.notify_all();
    impl_->idle_cv.notify_all();
    impl_->startup_cv.notify_all();
    for (auto& worker : impl_->workers)
        if (worker.joinable()) worker.join();
}

std::size_t WorkerPool::size() const noexcept { return impl_->workers.size(); }

const mmltk::common::system::ExecutionPolicySnapshot& WorkerPool::policy(std::size_t worker) const { return impl_->policies.at(worker); }
bool WorkerPool::idle() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->pending_tasks == 0U;
}

void WorkerPool::wait_idle() {
    std::unique_lock lock(impl_->mutex);
    impl_->idle_cv.wait(lock, [this] { return impl_->pending_tasks == 0U || impl_->startup_error != nullptr; });
    if (impl_->startup_error != nullptr) { std::rethrow_exception(impl_->startup_error); }
}

void WorkerPool::enqueue_detached(std::function<void()> task) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->shutdown) { throw std::runtime_error("worker pool is shut down"); }
        if (impl_->startup_error != nullptr) { std::rethrow_exception(impl_->startup_error); }
        impl_->push({.owned = std::move(task)});
        ++impl_->pending_tasks;
    }
    impl_->work_cv.notify_one();
}

void WorkerPool::enqueue_borrowed(void* context, std::size_t index, void (*call)(void*, std::size_t)) {
    if (!call) throw std::invalid_argument("borrowed worker task is empty");
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->startup_error) std::rethrow_exception(impl_->startup_error);
        if (impl_->shutdown) throw std::runtime_error("worker pool is shut down");
        impl_->push({.context = context, .index = index, .borrowed = call});
        ++impl_->pending_tasks;
    }
    impl_->work_cv.notify_one();
}

std::size_t WorkerPool::current_worker_index() const {
    if (!running_on_worker_thread()) throw std::logic_error("worker index requested outside its owning pool");
    return current_pool_index;
}

bool WorkerPool::running_on_worker_thread() const noexcept { return current_worker_pool == this; }

}  // namespace mmltk::common::concurrency
