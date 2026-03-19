#include "cpu_affinity.h"
#include "worker_pool.h"

#include <atomic>
#include <cassert>
#include <vector>

using namespace fastloader;

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
        pool.enqueue([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
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

} // namespace

int main() {
    test_cpu_affinity_helpers();
    test_enqueue_and_wait();
    test_parallel_for();
    test_nested_parallel_for_falls_back_inline();
    return 0;
}
