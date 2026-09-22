#pragma once
#include <algorithm>
#include <cstddef>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "src/common/math/checked_arithmetic.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/execution_policy.h"
namespace mmltk::common::concurrency {
template <typename Index, typename Func>
void parallel_for_range_indexed(
 const Index begin, const Index end, const int num_workers, const std::span<const int> cpu_affinity, Func&& func, const mmltk::common::system::ExecutionPlacement* placement = nullptr) {
 static_assert(std::is_integral_v<Index>, "parallel_for_range index must be integral");
 if (begin >= end) { return; }
 const Index total = end - begin;
 const int requested_workers = std::max(1, std::min(num_workers, mmltk::common::math::checked_cast<int>(total, "parallel range too large for worker count")));
 std::vector<int> worker_cpus(cpu_affinity.begin(), cpu_affinity.end());
 if (placement) {
  std::vector<int> local;
  for (int cpu : placement->cpus)
   if (std::ranges::find(worker_cpus, cpu) != worker_cpus.end()) local.push_back(cpu);
  worker_cpus = std::move(local);
 }
 if (worker_cpus.empty()) { throw std::runtime_error("parallel range requires a non-empty CPU affinity"); }
 const int worker_count = mmltk::common::system::clamp_worker_count_to_cpus(requested_workers, worker_cpus.size(), 0, 1);
 const auto topology = placement ? mmltk::common::system::NumaTopology{} : mmltk::common::system::NumaTopology::Capture();
 if (!placement) worker_cpus = mmltk::common::system::physical_core_order(topology, worker_cpus);
 auto node_for = [&](std::size_t worker) {
  if (placement) return placement->numa_node;
  const auto cpu = std::ranges::find(topology.cpus, worker_cpus[worker % worker_cpus.size()], &mmltk::common::system::CpuTopology::cpu);
  if (cpu == topology.cpus.end()) throw std::invalid_argument("parallel range CPU topology unavailable");
  return cpu->node;
 };
 if (worker_count == 1) {
  mmltk::common::system::ScopedExecutionPolicy policy({worker_cpus, {}, 0, node_for(0), -10, true});
  func(0, begin, end);
  policy.Restore();
  return;
 }
 const Index chunk = (total + static_cast<Index>(worker_count) - 1) / static_cast<Index>(worker_count);
 std::exception_ptr error;
 std::mutex error_mutex;
 std::vector<std::jthread> threads;
 threads.reserve(static_cast<std::size_t>(worker_count));
 for (int worker = 0; worker < worker_count; ++worker) {
  const Index chunk_begin = begin + static_cast<Index>(worker) * chunk;
  if (chunk_begin >= end) { break; }
  const Index chunk_end = std::min(end, chunk_begin + chunk);
  threads.emplace_back([&, worker, chunk_begin, chunk_end] {
   try {
    (void)mmltk::common::system::apply_worker_execution_policy(mmltk::common::system::ExecutionPolicyRequest{
     worker_cpus,
     "fl_par" + std::to_string(worker),
     static_cast<std::size_t>(worker),
     node_for(static_cast<std::size_t>(worker)),
     -10,
     true,
    });
    func(worker, chunk_begin, chunk_end);
   } catch (...) {
    std::lock_guard lock(error_mutex);
    if (!error) { error = std::current_exception(); }
   }
  });
 }
 for (auto& thread : threads) { thread.join(); }
 if (error) { std::rethrow_exception(error); }
}
template <typename Index, typename Func>
void parallel_for_range_indexed(const Index begin, const Index end, const int num_workers, Func&& func) {
 const std::vector<int> worker_cpus = mmltk::common::system::allowed_cpu_set();
 parallel_for_range_indexed(begin, end, num_workers, std::span<const int>(worker_cpus), std::forward<Func>(func));
}
template <typename Index, typename Func>
void parallel_for_range(const Index begin, const Index end, const int num_workers, Func&& func) {
 parallel_for_range_indexed(begin, end, num_workers, [&](int, const Index chunk_begin, const Index chunk_end) { func(chunk_begin, chunk_end); });
}
}  // namespace mmltk::common::concurrency
