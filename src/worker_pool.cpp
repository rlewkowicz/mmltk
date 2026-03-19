#include "worker_pool.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fastloader {

namespace {

thread_local const void* g_current_worker_pool = nullptr;

std::string worker_name_for_index(const std::string& prefix, size_t index) {
    if (prefix.empty()) {
        return {};
    }
    std::string name = prefix + std::to_string(index);
    if (name.size() > 15) {
        name.resize(15);
    }
    return name;
}

} // namespace

struct WorkerPool::Impl {
    std::mutex mutex;
    std::condition_variable work_cv;
    std::condition_variable idle_cv;
    std::condition_variable startup_cv;
    std::deque<std::function<void()>> tasks;
    std::vector<std::thread> workers;
    std::vector<int> cpu_affinity;
    std::string thread_name_prefix;
    size_t pending_tasks = 0;
    size_t started_workers = 0;
    bool shutdown = false;
    std::exception_ptr startup_error;

    void enqueue_task(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (shutdown) {
                throw std::runtime_error("worker pool is shut down");
            }
            if (startup_error != nullptr) {
                std::rethrow_exception(startup_error);
            }
            tasks.push_back(std::move(task));
            ++pending_tasks;
        }
        work_cv.notify_one();
    }

    void wait_for_startup(size_t worker_count) {
        std::unique_lock<std::mutex> lock(mutex);
        startup_cv.wait(lock, [&] {
            return started_workers == worker_count || startup_error != nullptr;
        });
        if (startup_error != nullptr) {
            std::rethrow_exception(startup_error);
        }
    }

    void request_shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            shutdown = true;
        }
        work_cv.notify_all();
        idle_cv.notify_all();
        startup_cv.notify_all();
    }

    void join_all() noexcept {
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }
};

WorkerPool::WorkerPool(size_t worker_count,
                       std::vector<int> cpu_affinity,
                       std::string thread_name_prefix)
    : impl_(std::make_unique<Impl>()) {
    if (worker_count == 0) {
        throw std::runtime_error("worker_count must be greater than zero");
    }
    impl_->cpu_affinity = cpu_affinity.empty() ? allowed_cpu_set() : std::move(cpu_affinity);
    impl_->thread_name_prefix = std::move(thread_name_prefix);
    impl_->workers.reserve(worker_count);
    for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        impl_->workers.emplace_back([this, worker_index] {
            try {
                pin_thread_to_cpu(impl_->cpu_affinity, worker_index);
                set_thread_name(worker_name_for_index(impl_->thread_name_prefix, worker_index));
            } catch (...) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                if (impl_->startup_error == nullptr) {
                    impl_->startup_error = std::current_exception();
                }
                impl_->shutdown = true;
                ++impl_->started_workers;
                impl_->startup_cv.notify_all();
                impl_->work_cv.notify_all();
                impl_->idle_cv.notify_all();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                ++impl_->started_workers;
            }
            impl_->startup_cv.notify_all();
            g_current_worker_pool = impl_.get();
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(impl_->mutex);
                    impl_->work_cv.wait(lock, [this] {
                        return impl_->shutdown || !impl_->tasks.empty();
                    });
                    if (impl_->shutdown && impl_->tasks.empty()) {
                        g_current_worker_pool = nullptr;
                        return;
                    }
                    task = std::move(impl_->tasks.front());
                    impl_->tasks.pop_front();
                }
                try {
                    task();
                } catch (...) {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    impl_->shutdown = true;
                    if (impl_->startup_error == nullptr) {
                        impl_->startup_error = std::current_exception();
                    }
                    impl_->work_cv.notify_all();
                    impl_->idle_cv.notify_all();
                    impl_->startup_cv.notify_all();
                }
                {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    if (impl_->pending_tasks == 0) {
                        impl_->shutdown = true;
                        if (impl_->startup_error == nullptr) {
                            impl_->startup_error = std::make_exception_ptr(
                                std::runtime_error("worker pool internal task accounting underflow"));
                        }
                        impl_->work_cv.notify_all();
                        impl_->idle_cv.notify_all();
                        impl_->startup_cv.notify_all();
                        g_current_worker_pool = nullptr;
                        return;
                    }
                    --impl_->pending_tasks;
                    if (impl_->pending_tasks == 0) {
                        impl_->idle_cv.notify_all();
                    }
                }
            }
        });
    }
    try {
        impl_->wait_for_startup(worker_count);
    } catch (...) {
        impl_->request_shutdown();
        impl_->join_all();
        throw;
    }
}

WorkerPool::~WorkerPool() {
    if (!impl_) {
        return;
    }
    impl_->request_shutdown();
    impl_->join_all();
}

size_t WorkerPool::size() const noexcept {
    return impl_ ? impl_->workers.size() : 0;
}

void WorkerPool::wait_idle() {
    if (!impl_) {
        return;
    }
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->idle_cv.wait(lock, [this] {
        return impl_->pending_tasks == 0 || impl_->startup_error != nullptr;
    });
    if (impl_->startup_error != nullptr) {
        std::rethrow_exception(impl_->startup_error);
    }
}

void WorkerPool::enqueue_task(std::function<void()> task) {
    impl_->enqueue_task(std::move(task));
}

bool WorkerPool::running_on_worker_thread() const noexcept {
    return impl_ && g_current_worker_pool == impl_.get();
}

} // namespace fastloader
