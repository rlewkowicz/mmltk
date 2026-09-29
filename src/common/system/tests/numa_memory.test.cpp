#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/common/system/tests/denied_syscall.h"
#include <catch2/catch_test_macros.hpp>
#include <sys/syscall.h>
#include <limits>
#include <algorithm>
#include <vector>
#include <system_error>
namespace {
using namespace mmltk::common::system;
TEST_CASE("Owned local pages keep high-water storage and verify placement", "[common][system][numa-memory]") {
 const auto facts = NumaTopology::Capture();
 NumaMemory memory(facts.permitted_nodes.front());
 CHECK(memory.data() == nullptr);
 memory.ensure_bytes(0);
 CHECK(memory.data() == nullptr);
 const auto local = std::ranges::find(facts.nodes, memory.node(), &MemoryNode::node);
 REQUIRE(local != facts.nodes.end());
 CHECK_THROWS_AS(memory.ensure_bytes(local->bytes + host_page_size()), std::bad_alloc);
 CHECK(memory.data() == nullptr);
 memory.ensure_bytes(17);
 auto* first = memory.data();
 REQUIRE(first);
 CHECK(memory.capacity_bytes() == host_page_size());
 memory.ensure_bytes(16);
 CHECK(memory.data() == first);
 memory.verify();
 memory.ensure_bytes(host_page_size() + 1);
 CHECK(memory.capacity_bytes() == host_page_size() * 2);
 memory.verify();
 const auto stable = memory.data();
 CHECK_THROWS_AS(memory.ensure_bytes(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
 CHECK(memory.data() == stable);
 NumaMemory moved(std::move(memory));
 CHECK(memory.data() == nullptr);
 CHECK(moved.data() == stable);
}
TEST_CASE("Node-bound PMR backs reusable solver vectors and over-aligned objects", "[common][system][numa-memory]") {
 const auto facts = NumaTopology::Capture();
 NumaMemoryResource resource(facts.permitted_nodes.front());
 std::pmr::vector<double> values(&resource);
 values.reserve(257);
 values.resize(100);
 auto* first = values.data();
 values.clear();
 values.resize(256);
 CHECK(values.data() == first);
 auto* aligned = resource.allocate(8, host_page_size() * 2);
 CHECK(reinterpret_cast<std::uintptr_t>(aligned) % (host_page_size() * 2) == 0);
 resource.deallocate(aligned, 8, host_page_size() * 2);
}
TEST_CASE("Denied binding or page verification never publishes host storage", "[common][system][numa-memory]") {
 for (const int call : {SYS_mbind, SYS_move_pages}) {
  CHECK(test_support::with_denied_syscall(call, [] {
   NumaMemory memory(NumaTopology::Capture().permitted_nodes.front());
   try {
    memory.ensure_bytes(1);
   } catch (const std::system_error& error) { return error.code().value() == EPERM && !memory.data(); }
   return false;
  }) == 0);
 }
}
TEST_CASE("Prepared memory policy capture reuses storage and validates permitted nodes", "[common][system][numa-memory]") {
 const auto facts = NumaTopology::Capture();
 const auto before = capture_memory_policy();
 auto captured = before;
 auto* const storage = captured.mask.data();
 const auto capacity = captured.mask.capacity();
 const auto desired = prepare_bound_memory_policy(facts.permitted_nodes.front());
 CHECK(bound_memory_node(desired) == facts.permitted_nodes.front());
 for (unsigned i = 0; i < 4; ++i) {
  capture_memory_policy(captured);
  CHECK(captured.mode == before.mode);
  CHECK(captured.mask == before.mask);
  CHECK(captured.mask.data() == storage);
  CHECK(captured.mask.capacity() == capacity);
 }
 CHECK_THROWS_AS(prepare_bound_memory_policy(-1), std::invalid_argument);
 CHECK_THROWS_AS(prepare_bound_memory_policy(std::numeric_limits<int>::max()), std::invalid_argument);
 MemoryPolicy empty;
 CHECK_THROWS_AS(capture_memory_policy(empty), std::invalid_argument);
 CHECK(test_support::with_denied_syscall(SYS_get_mempolicy, [&] {
  try {
   capture_memory_policy(captured);
  } catch (const std::system_error& error) {
   if (error.code().value() != EPERM || captured.mask.data() != storage) return false;
   try {
    (void)prepare_bound_memory_policy(facts.permitted_nodes.front());
   } catch (const std::system_error& denied) { return denied.code().value() == EPERM; }
  }
  return false;
 }) == 0);
}
}  // namespace
