#pragma once

#include "worker_pool.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fastloader::runtime {

class TaskCancellation {
public:
    TaskCancellation();

    void cancel() const noexcept;
    bool cancelled() const noexcept;
    bool finished() const noexcept;

private:
    struct State {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> finished{false};
    };

    explicit TaskCancellation(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;

    template <typename WorkFn, typename SuccessFn, typename ErrorFn>
    friend TaskCancellation submit_background_task(class BackgroundExecutor& executor,
                                                   class UiCallbackQueue& ui_queue,
                                                   WorkFn&& work,
                                                   SuccessFn&& on_success,
                                                   ErrorFn&& on_error);
};

class UiCallbackQueue {
public:
    void post(std::function<void()> callback);
    std::size_t drain(std::size_t max_callbacks = 0);
    bool empty() const;

private:
    mutable std::mutex mutex_;
    std::deque<std::function<void()>> callbacks_;
};

class BackgroundExecutor {
public:
    explicit BackgroundExecutor(std::size_t worker_count = 2,
                                std::vector<int> cpu_affinity = {},
                                std::string thread_name_prefix = "guibg");

    std::size_t size() const noexcept;
    void wait_idle();

    template <typename Fn>
    void enqueue(Fn&& fn) {
        worker_pool_.enqueue(std::forward<Fn>(fn));
    }

private:
    fastloader::WorkerPool worker_pool_;

    template <typename WorkFn, typename SuccessFn, typename ErrorFn>
    friend TaskCancellation submit_background_task(BackgroundExecutor& executor,
                                                   UiCallbackQueue& ui_queue,
                                                   WorkFn&& work,
                                                   SuccessFn&& on_success,
                                                   ErrorFn&& on_error);
};

namespace detail {

inline std::string exception_message(const std::exception& error) {
    return error.what();
}

inline std::string exception_message(...) {
    return "unknown background task failure";
}

template <typename WorkFn>
using work_result_t = std::invoke_result_t<WorkFn>;

template <typename WorkFn>
constexpr bool kWorkReturnsVoid = std::is_void_v<work_result_t<WorkFn>>;

} // namespace detail

template <typename WorkFn, typename SuccessFn, typename ErrorFn>
TaskCancellation submit_background_task(BackgroundExecutor& executor,
                                        UiCallbackQueue& ui_queue,
                                        WorkFn&& work,
                                        SuccessFn&& on_success,
                                        ErrorFn&& on_error) {
    auto state = std::make_shared<TaskCancellation::State>();
    TaskCancellation token(state);
    auto work_fn = std::make_shared<std::decay_t<WorkFn>>(std::forward<WorkFn>(work));
    auto success_fn = std::make_shared<std::decay_t<SuccessFn>>(std::forward<SuccessFn>(on_success));
    auto error_fn = std::make_shared<std::decay_t<ErrorFn>>(std::forward<ErrorFn>(on_error));

    executor.enqueue([state, &ui_queue, work_fn, success_fn, error_fn]() mutable {
        try {
            if constexpr (detail::kWorkReturnsVoid<std::decay_t<WorkFn>>) {
                (*work_fn)();
                if (state->cancelled.load(std::memory_order_relaxed)) {
                    state->finished.store(true, std::memory_order_relaxed);
                    return;
                }
                ui_queue.post([state, success_fn]() mutable {
                    if (!state->cancelled.load(std::memory_order_relaxed)) {
                        (*success_fn)();
                    }
                    state->finished.store(true, std::memory_order_relaxed);
                });
                return;
            } else {
                using Result = detail::work_result_t<std::decay_t<WorkFn>>;
                auto result = std::make_shared<Result>((*work_fn)());
                if (state->cancelled.load(std::memory_order_relaxed)) {
                    state->finished.store(true, std::memory_order_relaxed);
                    return;
                }
                ui_queue.post([state, success_fn, result]() mutable {
                    if (!state->cancelled.load(std::memory_order_relaxed)) {
                        (*success_fn)(std::move(*result));
                    }
                    state->finished.store(true, std::memory_order_relaxed);
                });
                return;
            }
        } catch (const std::exception& error) {
            const auto message = std::make_shared<std::string>(detail::exception_message(error));
            if (state->cancelled.load(std::memory_order_relaxed)) {
                state->finished.store(true, std::memory_order_relaxed);
                return;
            }
            ui_queue.post([state, error_fn, message]() mutable {
                if (!state->cancelled.load(std::memory_order_relaxed)) {
                    (*error_fn)(*message);
                }
                state->finished.store(true, std::memory_order_relaxed);
            });
        } catch (...) {
            const auto message = std::make_shared<std::string>(detail::exception_message());
            if (state->cancelled.load(std::memory_order_relaxed)) {
                state->finished.store(true, std::memory_order_relaxed);
                return;
            }
            ui_queue.post([state, error_fn, message]() mutable {
                if (!state->cancelled.load(std::memory_order_relaxed)) {
                    (*error_fn)(*message);
                }
                state->finished.store(true, std::memory_order_relaxed);
            });
        }
    });
    return token;
}

} // namespace fastloader::runtime
