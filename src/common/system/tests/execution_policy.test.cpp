#include "src/common/system/tests/numa_topology_test_support.h"
#include "src/common/system/execution_policy.h"
#include "src/common/system/cpu_affinity.h"
#include "src/common/system/tests/denied_syscall.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/syscall.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <system_error>
#include <algorithm>
#include "src/common/concurrency/worker_pool.h"
namespace {
using namespace mmltk::common::system;
NumaTopology topology() {
 return {.permitted_cpus = {2, 7, 19, 31, 42}, .permitted_nodes = {0, 3}, .cpus = {{2, 0, 0, 0}, {7, 0, 0, 0}, {19, 0, 0, 1}, {31, 3, 1, 0}, {42, 3, 1, 1}}, .nodes = {{0, 4096}, {3, 8192}}};
}
TEST_CASE("GPU placement honors sparse node IDs, SMT and explicit eligibility", "[common][system][topology]") {
 auto facts = topology();
 CHECK(resolve_placement(facts, 0).cpus == std::vector<int>{2, 19, 7});
 CHECK(resolve_placement(facts, 3).cpus == std::vector<int>{31, 42});
 CHECK_THROWS_AS(resolve_placement(facts, -1), std::invalid_argument);
 CHECK(resolve_placement(facts, -1, 3).numa_node == 3);
 CHECK_THROWS_AS(resolve_placement(facts, 0, 3), std::invalid_argument);
 CHECK_THROWS_AS(resolve_placement(facts, 0, -2), std::invalid_argument);
 const std::vector<int> eligibility{7, 31};
 CHECK(resolve_placement(facts, 0, -1, eligibility).cpus == std::vector<int>{7});
 facts.permitted_cpus = {31};
 CHECK_THROWS_AS(resolve_placement(facts, 0), std::invalid_argument);
 facts.permitted_nodes = {0};
 CHECK_THROWS_AS(resolve_placement(facts, 3), std::invalid_argument);
}
TEST_CASE("Only genuinely single-node machines resolve unknown GPU locality automatically", "[common][system][topology]") {
 auto facts = topology();
 facts.nodes.resize(1);
 CHECK(resolve_placement(facts, -1).numa_node == 0);
 facts.nodes[0].bytes = 0;
 CHECK_THROWS_AS(resolve_placement(facts, -1), std::invalid_argument);
}
TEST_CASE("Scoped boundary verifies effective placement and restores caller policy", "[common][system][policy]") {
 const auto facts = NumaTopology::Capture();
 const auto selected = mmltk::common::system::test_support::first_permitted_cpu(facts);
 const auto cpu = selected.cpu;
 const auto node = selected.node;
 const auto before = capture_execution_policy_snapshot();
 {
  ScopedExecutionPolicy policy({{cpu}, {}, 0, node, -10, true});
  const auto active = capture_execution_policy_snapshot();
  CHECK(active.affinity == std::vector<int>{cpu});
  CHECK(active.numa_node == node);
  CHECK(active.nice_value <= -10);
  CHECK(active.scheduler_policy == SCHED_OTHER);
  CHECK(active.io_class == IOPRIO_CLASS_BE);
  CHECK(active.io_priority_data == 0);
 }
 const auto after = capture_execution_policy_snapshot();
 CHECK(after.affinity == before.affinity);
 CHECK(after.nice_value == before.nice_value);
 CHECK(after.memory_policy.mode == before.memory_policy.mode);
 CHECK(after.memory_policy.mask == before.memory_policy.mask);
 CHECK(after.io_class == before.io_class);
 CHECK(after.io_priority_data == before.io_priority_data);
}
TEST_CASE("Denied priority and NUMA policy fail required worker startup", "[common][system][policy]") {
 for (const int call : {SYS_setpriority, SYS_set_mempolicy, SYS_ioprio_set}) {
  CHECK(test_support::with_denied_syscall(call, [] {
   const auto facts = NumaTopology::Capture();
   const auto selected = mmltk::common::system::test_support::first_permitted_cpu(facts);
   const auto cpu = selected.cpu;
   const auto node = selected.node;
   try {
    (void)apply_worker_execution_policy({{cpu}, {}, 0, node, -10, true});
   } catch (const std::system_error& error) { return error.code().value() == EPERM; }
   return false;
  }) == 0);
 }
}
}  // namespace
TEST_CASE("Synchronous policy failure restores every policy already changed", "[common][system][policy]") {
 CHECK(mmltk::common::system::test_support::with_denied_syscall(SYS_ioprio_set, [] {
  using namespace mmltk::common::system;
  const auto before = capture_execution_policy_snapshot();
  const auto topology = NumaTopology::Capture();
  const auto selected = mmltk::common::system::test_support::first_permitted_cpu(topology);
  const auto cpu = selected.cpu;
  const auto node = selected.node;
  try {
   ScopedExecutionPolicy scope({{cpu}, "denied-scope", 0, node, -10, true});
  } catch (const std::system_error&) {
   const auto after = capture_execution_policy_snapshot();
   return after.affinity == before.affinity && after.nice_value == before.nice_value && after.memory_policy.mode == before.memory_policy.mode &&
          after.memory_policy.mask == before.memory_policy.mask && after.thread_name == before.thread_name;
  }
  return false;
 }) == 0);
}
TEST_CASE("Policy denial rolls back partially constructed worker pools", "[common][system][policy]") {
 CHECK(mmltk::common::system::test_support::with_denied_syscall(SYS_setpriority, [] {
  try {
   mmltk::common::concurrency::WorkerPool workers(4);
  } catch (const std::system_error& error) { return error.code().value() == EPERM; }
  return false;
 }) == 0);
}
