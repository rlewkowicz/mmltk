#include "common_utils.h"
#include "cpu_affinity.h"
#include "execution_policy.h"
#include "worker_pool.h"

#include <atomic>
#include "support/catch2_compat.hpp"
#include <mutex>
#include <set>
#include <vector>

using namespace mmltk;

namespace {

void test_cpu_affinity_helpers() {
    const std::vector<int> allowed = allowed_cpu_set();
    assert(!allowed.empty());
    assert(resolve_cpu_affinity("") == allowed);

    const std::vector<int> parsed = parse_cpu_list("3,1-2,2,5");
    assert((parsed == std::vector<int>{1, 2, 3, 5}));
    assert(format_cpu_list(parsed) == "1-3,5");

    const std::string allowed_spec = format_cpu_list(allowed);
    assert(!allowed_spec.empty());
    assert(resolve_cpu_affinity(allowed_spec) == allowed);
}

void test_enqueue_and_wait() {
    WorkerPool pool(2);
    std::atomic<int> counter{0};
    for (int i = 0; i < 32; ++i) {
        pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.wait_idle();
    assert(counter.load(std::memory_order_relaxed) == 32);
}

void test_parallel_for() {
    WorkerPool pool(4);
    std::vector<int> values(257, 0);
    pool.parallel_for<size_t>(0, values.size(), 4, [&](size_t begin, size_t end) {
        for (size_t index = begin; index < end; ++index) {
            values[index] = static_cast<int>(index * 2);
        }
    });
    for (size_t index = 0; index < values.size(); ++index) {
        assert(values[index] == static_cast<int>(index * 2));
    }
}

void test_nested_parallel_for_falls_back_inline() {
    WorkerPool pool(2);
    std::atomic<int> completed{0};
    auto outer = pool.enqueue([&] {
        pool.parallel_for<int>(0, 8, 2, [&](int begin, int end) {
            for (int index = begin; index < end; ++index) {
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    });
    outer.get();
    assert(completed.load(std::memory_order_relaxed) == 8);
}

void test_worker_count_clamp_helper() {
    assert(clamp_worker_count_to_cpus(32, 8, 0, 1) == 8);
    assert(clamp_worker_count_to_cpus(8, 8, 1, 1) == 7);
    assert(clamp_worker_count_to_cpus(1, 1, 1, 1) == 1);
    assert(clamp_worker_count_to_cpus(16, 2, 0, 3) == 3);
}

void test_worker_pool_clamps_and_pins_threads() {
    const std::vector<int> allowed = allowed_cpu_set();
    assert(!allowed.empty());

    const size_t cpu_count = std::min<size_t>(allowed.size(), 3);
    std::vector<int> subset(allowed.begin(), allowed.begin() + static_cast<std::ptrdiff_t>(cpu_count));
    WorkerPool pool(subset.size() + 4, subset, "twp");
    assert(pool.size() == subset.size());

    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::future<std::vector<int>>> futures;
    futures.reserve(pool.size());
    for (size_t index = 0; index < pool.size(); ++index) {
        futures.push_back(pool.enqueue([&] {
            std::vector<int> mask = allowed_cpu_set();
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return mask;
        }));
    }

    while (ready.load(std::memory_order_acquire) != static_cast<int>(pool.size())) {
        std::this_thread::yield();
    }
    release.store(true, std::memory_order_release);

    std::set<int> observed;
    for (auto& future : futures) {
        const std::vector<int> mask = future.get();
        assert(mask.size() == 1);
        assert(std::find(subset.begin(), subset.end(), mask.front()) != subset.end());
        observed.insert(mask.front());
    }
    assert(observed.size() == pool.size());
}

void test_parallel_for_range_threads_are_pinned() {
    const std::vector<int> allowed = allowed_cpu_set();
    const int requested_workers = std::min<int>(4, static_cast<int>(allowed.size()));
    assert(requested_workers >= 1);

    std::vector<std::vector<int>> masks(static_cast<size_t>(requested_workers));
    parallel_for_range_indexed<int>(0, requested_workers, requested_workers, [&](int worker, int, int) {
        masks[static_cast<size_t>(worker)] = allowed_cpu_set();
    });

    std::set<int> observed;
    for (const auto& mask : masks) {
        assert(mask.size() == 1);
        observed.insert(mask.front());
    }
    assert(observed.size() == masks.size());
}

}  

MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_cpu_affinity_helpers);
MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_enqueue_and_wait);
MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_parallel_for);
MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_nested_parallel_for_falls_back_inline);
MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_worker_count_clamp_helper);
MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_worker_pool_clamps_and_pins_threads);
MMLTK_REGISTER_TEST_CASE("[core][worker_pool]", test_parallel_for_range_threads_are_pinned);
