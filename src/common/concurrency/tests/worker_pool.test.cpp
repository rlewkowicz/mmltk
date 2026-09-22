#include "src/common/system/tests/numa_topology_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/common/concurrency/worker_pool.h"
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"
namespace {
using mmltk::common::concurrency::parallel_for_range_indexed;
using mmltk::common::concurrency::WorkerPool;
using mmltk::common::system::allowed_cpu_set;
using mmltk::common::system::clamp_worker_count_to_cpus;
using mmltk::common::system::format_cpu_list;
using mmltk::common::system::parse_cpu_list;
using mmltk::common::system::resolve_cpu_affinity;
TEST_CASE("CPU affinity helpers preserve the allowed set", "[common][concurrency][worker_pool]") {
 const std::vector<int> allowed = allowed_cpu_set();
 REQUIRE_FALSE(allowed.empty());
 REQUIRE(resolve_cpu_affinity("") == allowed);
 const std::vector<int> parsed = parse_cpu_list("3,1-2,2,5");
 REQUIRE(parsed == std::vector<int>{1, 2, 3, 5});
 REQUIRE(format_cpu_list(parsed) == "1-3,5");
 const std::string allowed_spec = format_cpu_list(allowed);
 REQUIRE_FALSE(allowed_spec.empty());
 REQUIRE(resolve_cpu_affinity(allowed_spec) == allowed);
}
TEST_CASE("WorkerPool enqueues work and waits for idle", "[common][concurrency][worker_pool]") {
 WorkerPool pool(2);
 std::atomic<int> counter{0};
 for (int i = 0; i < 32; ++i) {
  pool.enqueue([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
 }
 pool.wait_idle();
 REQUIRE(counter.load(std::memory_order_relaxed) == 32);
}
TEST_CASE("WorkerPool partitions a parallel range", "[common][concurrency][worker_pool]") {
 WorkerPool pool(4);
 std::vector<int> values(257, 0);
 pool.parallel_for<std::size_t>(0, values.size(), 4, [&](const std::size_t begin, const std::size_t end) {
  for (std::size_t index = begin; index < end; ++index) { values[index] = static_cast<int>(index * 2); }
 });
 for (std::size_t index = 0; index < values.size(); ++index) { REQUIRE(values[index] == static_cast<int>(index * 2)); }
}
TEST_CASE("nested WorkerPool ranges execute inline", "[common][concurrency][worker_pool]") {
 WorkerPool pool(2);
 std::atomic<int> completed{0};
 auto outer = pool.enqueue([&] {
  pool.parallel_for<int>(0, 8, 2, [&](const int begin, const int end) {
   for (int index = begin; index < end; ++index) { completed.fetch_add(1, std::memory_order_relaxed); }
  });
 });
 outer.get();
 REQUIRE(completed.load(std::memory_order_relaxed) == 8);
}
TEST_CASE("worker count clamps to available CPUs", "[common][concurrency][worker_pool]") {
 REQUIRE(clamp_worker_count_to_cpus(32, 8, 0, 1) == 8);
 REQUIRE(clamp_worker_count_to_cpus(8, 8, 1, 1) == 7);
 REQUIRE(clamp_worker_count_to_cpus(1, 1, 1, 1) == 1);
 REQUIRE(clamp_worker_count_to_cpus(16, 2, 0, 3) == 3);
}
TEST_CASE("WorkerPool clamps and pins worker threads", "[common][concurrency][worker_pool]") {
 const std::vector<int> allowed = allowed_cpu_set();
 REQUIRE_FALSE(allowed.empty());
 const std::size_t cpu_count = std::min<std::size_t>(allowed.size(), 3);
 std::vector<int> subset(allowed.begin(), allowed.begin() + static_cast<std::ptrdiff_t>(cpu_count));
 WorkerPool pool(subset.size() + 4, subset, "twp");
 REQUIRE(pool.size() == subset.size());
 mmltk::testsupport::TestGate gate{"pinned worker mask captured"};
 std::vector<std::future<std::vector<int>>> futures;
 futures.reserve(pool.size());
 mmltk::testsupport::ScopedTestCleanup release_workers{[&] {
  gate.Release();
  try {
   pool.wait_idle();
  } catch (...) {}
 }};
 for (std::size_t index = 0; index < pool.size(); ++index) {
  futures.push_back(pool.enqueue([&] {
   std::vector<int> mask = allowed_cpu_set();
   gate.receipt().ArriveAndWait();
   return mask;
  }));
 }
 REQUIRE(gate.WaitEntered(std::chrono::seconds{2}, pool.size()));
 gate.Release();
 std::set<int> observed;
 for (auto& future : futures) {
  const std::vector<int> mask = mmltk::testsupport::await_test_future(future, "released pinned worker");
  REQUIRE(mask.size() == 1);
  REQUIRE(std::find(subset.begin(), subset.end(), mask.front()) != subset.end());
  observed.insert(mask.front());
 }
 REQUIRE(observed.size() == pool.size());
 CHECK(gate.WaitSettled(std::chrono::seconds{2}, pool.size()));
}
TEST_CASE("parallel range workers are pinned", "[common][concurrency][worker_pool]") {
 const std::vector<int> allowed = allowed_cpu_set();
 const int requested_workers = std::min<int>(4, static_cast<int>(allowed.size()));
 REQUIRE(requested_workers >= 1);
 std::vector<std::vector<int>> masks(static_cast<std::size_t>(requested_workers));
 parallel_for_range_indexed<int>(0, requested_workers, requested_workers, [&](const int worker, int, int) { masks[static_cast<std::size_t>(worker)] = allowed_cpu_set(); });
 std::set<int> observed;
 for (const auto& mask : masks) {
  REQUIRE(mask.size() == 1);
  observed.insert(mask.front());
 }
 REQUIRE(observed.size() == masks.size());
}
TEST_CASE("WorkerPool detached failures wake waiters and close admission", "[common][concurrency][worker_pool]") {
 WorkerPool pool(1);
 pool.enqueue_detached([] { throw std::runtime_error("detached work failed"); });
 REQUIRE_THROWS_AS(pool.wait_idle(), std::runtime_error);
 REQUIRE_THROWS_AS(pool.enqueue_detached([] {}), std::runtime_error);
}
TEST_CASE("WorkerPool preserves FIFO across queue wrap and high-water growth", "[common][concurrency][worker_pool]") {
 WorkerPool pool(1U, {}, "queue-order", 3U);
 std::vector<int> observed;
 mmltk::testsupport::ScopedTestCleanup settle_fifo{[&] {
  try {
   pool.wait_idle();
  } catch (...) {}
 }};
 std::vector<int> expected;
 for (int round = 0; round < 4; ++round) {
  mmltk::testsupport::TestGate gate{"FIFO worker admission"};
  pool.enqueue_detached([receipt = gate.receipt()] { receipt.ArriveAndWait(); });
  REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
  const int count = round == 1 ? 19 : 7;
  for (int index = 0; index < count; ++index) {
   const int value = round * 100 + index;
   expected.push_back(value);
   pool.enqueue_detached([&observed, value] { observed.push_back(value); });
  }
  gate.Release();
  pool.wait_idle();
  REQUIRE(observed == expected);
 }
}
TEST_CASE("WorkerPool shutdown drains previously admitted FIFO work after detached failure", "[common][concurrency][worker_pool]") {
 std::future<int> accepted;
 {
  WorkerPool pool(1U, {}, "queue-failure", 1U);
  mmltk::testsupport::TestGate gate{"FIFO worker admission"};
  pool.enqueue_detached([receipt = gate.receipt()] {
   receipt.ArriveAndWait();
   throw std::runtime_error("detached failure before accepted work");
  });
  REQUIRE(gate.WaitEntered(std::chrono::seconds{2}));
  accepted = pool.enqueue([] { return 31; });
  gate.Release();
  REQUIRE_THROWS_AS(pool.wait_idle(), std::runtime_error);
 }
 REQUIRE(accepted.get() == 31);
}
TEST_CASE("WorkerPool result failures remain owned by their futures", "[common][concurrency][worker_pool]") {
 WorkerPool pool(1);
 auto failed = pool.enqueue([]() -> int { throw std::runtime_error("result work failed"); });
 REQUIRE_THROWS_AS(failed.get(), std::runtime_error);
 REQUIRE(pool.enqueue([] { return 17; }).get() == 17);
 REQUIRE_NOTHROW(pool.wait_idle());
}
}  // namespace
TEST_CASE("Borrowed worker records reuse bounded queues and preserve task failures", "[common][concurrency][worker_pool]") {
 WorkerPool pool(1, {}, "borrowed", 8);
 struct Counter {
  std::atomic<std::size_t> sum{0};
 } counter;
 auto add = [](void* context, std::size_t index) { static_cast<Counter*>(context)->sum.fetch_add(index); };
 for (std::size_t round = 0; round < 20; ++round) {
  for (std::size_t index = 0; index < 8; ++index) pool.enqueue_borrowed(&counter, index, add);
  pool.wait_idle();
 }
 CHECK(counter.sum.load() == 20U * 28U);
 auto failed = pool.enqueue([] { throw std::runtime_error("future-owned failure"); });
 CHECK_THROWS_AS(failed.get(), std::runtime_error);
 CHECK_NOTHROW(pool.wait_idle());
}
TEST_CASE("Child pools use immutable placement after the creator is pinned", "[common][concurrency][worker_pool]") {
 using namespace mmltk::common::system;
 const auto topology = NumaTopology::Capture();
 const auto first = mmltk::common::system::test_support::first_permitted_cpu(topology);
 const auto placement = resolve_placement(topology, first.node);
 WorkerPool parent(1, placement.cpus, "parent", 1, &placement);
 const auto observed = parent
                        .enqueue([&] {
                         WorkerPool children(placement.cpus.size(), placement.cpus, "child", 1, &placement);
                         auto snapshot = children.enqueue([] { return capture_execution_policy_snapshot(); }).get();
                         return std::pair{children.size(), snapshot};
                        })
                        .get();
 CHECK(observed.first == placement.cpus.size());
 CHECK(observed.second.numa_node == placement.numa_node);
 CHECK(observed.second.nice_value <= -10);
}
// CLEANUP-IGNORE: Overlapping worker budgets and nested child placement independently capture their creator topology.
TEST_CASE("Placed worker budgets overlap one local CPU with stable assignments", "[common][concurrency][worker_pool]") {
 using namespace mmltk::common::system;
 const auto topology = NumaTopology::Capture();
 const auto first = mmltk::common::system::test_support::first_permitted_cpu(topology);
 auto placement = resolve_placement(topology, first.node);
 placement.cpus.resize(1);
 WorkerPool pool(3, placement.cpus, "overlap", 3, &placement);
 REQUIRE(pool.size() == 3);
 CHECK_THROWS_AS(pool.current_worker_index(), std::logic_error);
 const auto index = pool.enqueue([&pool] { return pool.current_worker_index(); }).get();
 CHECK(index < pool.size());
 for (std::size_t worker = 0; worker < pool.size(); ++worker) {
  CHECK(pool.policy(worker).affinity == placement.cpus);
  CHECK(pool.policy(worker).numa_node == placement.numa_node);
 }
}
