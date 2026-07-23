#pragma once

#include "dataset_compiler.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace mmltk {

template <typename Observer>
void compile_with_progress(const DatasetCompilePlan& plan, const size_t split_index, Observer&& observer,
                           const std::chrono::milliseconds interval = std::chrono::milliseconds(100)) {
    CompileTelemetry telemetry(plan.splits.at(split_index).image_count);
    std::mutex mutex;
    std::condition_variable finished_cv;
    bool finished = false;
    std::exception_ptr observer_error;

    std::thread observer_thread([&] {
        try {
            while (true) {
                std::invoke(observer, telemetry.snapshot());
                std::unique_lock lock(mutex);
                if (finished_cv.wait_for(lock, interval, [&] { return finished; })) {
                    lock.unlock();
                    std::invoke(observer, telemetry.snapshot());
                    return;
                }
            }
        } catch (...) {
            observer_error = std::current_exception();
        }
    });

    std::exception_ptr compile_error;
    try {
        DatasetCompiler::compile(plan, split_index, &telemetry);
    } catch (...) {
        compile_error = std::current_exception();
    }
    {
        std::lock_guard lock(mutex);
        finished = true;
    }
    finished_cv.notify_one();
    observer_thread.join();
    if (compile_error != nullptr) {
        std::rethrow_exception(compile_error);
    }
    if (observer_error != nullptr) {
        std::rethrow_exception(observer_error);
    }
}

}  
