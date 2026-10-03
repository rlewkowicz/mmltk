#include "src/common/system/numa_memory.h"
#include <numa.h>
#include <numaif.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
namespace mmltk::common::system {
namespace {
constexpr std::size_t word_bits = sizeof(unsigned long) * 8U;
void checked(long result, const char* operation) {
 if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}
void verify_pages(void* data, std::size_t bytes, int node, NumaMemory::Lock lock) {
 std::array<void*, 256> pages{};
 std::array<int, 256> status{};
 const auto page = host_page_size();
 for (std::size_t offset = 0; offset < bytes;) {
  const auto count = std::min(pages.size(), (bytes - offset) / page);
  for (std::size_t i = 0; i < count; ++i) pages[i] = static_cast<std::byte*>(data) + offset + i * page;
  checked(::move_pages(0, count, pages.data(), nullptr, status.data(), 0), "verify host page NUMA placement");
  for (std::size_t i = 0; i < count; ++i) {
   // mlock prevents eviction, but Linux may still migrate a resident page.
   // The non-faulting batch query sees its temporary migration entry as
   // ENOENT. Resolve that locked page through the fault-aware node query;
   // this still verifies actual placement and does not retry other failures.
   if (status[i] == -ENOENT && lock == NumaMemory::Lock::Pages)
    checked(::get_mempolicy(&status[i], nullptr, 0, pages[i], MPOL_F_NODE | MPOL_F_ADDR), "resolve locked host page NUMA placement");
   if (status[i] == node) continue;
   const auto operation = "verify host page at byte " + std::to_string(offset + i * page) + " of " + std::to_string(bytes) + " on NUMA node " + std::to_string(node);
   if (status[i] < 0) throw std::system_error(-status[i], std::generic_category(), operation);
   throw std::runtime_error(operation + ": resident on NUMA node " + std::to_string(status[i]));
  }
  offset += count * page;
 }
}
void* allocate_pages(std::size_t bytes, std::size_t alignment, int node, NumaMemory::Lock lock = NumaMemory::Lock::None) {
 const auto policy = prepare_bound_memory_policy(node);
 // MemFree excludes reclaimable file cache and cannot decide allocation
 // admission while a compiler is reading and writing large mapped datasets.
 // Reject physically impossible extents here; prefaulting below asks Linux
 // to reclaim pages and reports an actual allocation failure transactionally.
 const auto total = numa_node_size64(node, nullptr);
 if (total < 0 || bytes > static_cast<std::uint64_t>(total)) throw std::bad_alloc();
 const auto page = host_page_size();
 const auto extra = alignment > page ? alignment : 0U;
 if (bytes > std::numeric_limits<std::size_t>::max() - extra) throw std::bad_alloc();
 void* mapping = ::mmap(nullptr, bytes + extra, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
 if (mapping == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "map local host storage");
 if (extra) {
  const auto address = reinterpret_cast<std::uintptr_t>(mapping);
  const auto aligned = (address + alignment - 1U) & ~(alignment - 1U);
  const auto prefix = aligned - address;
  if (prefix) ::munmap(mapping, prefix);
  const auto suffix = extra - prefix;
  if (suffix) ::munmap(reinterpret_cast<void*>(aligned + bytes), suffix);
  mapping = reinterpret_cast<void*>(aligned);
 }
 try {
  checked(::mbind(mapping, bytes, policy.mode, policy.mask.data(), policy.mask.size() * word_bits, 0), "bind owned host storage");
  // Populate writable pages before locking so allocation errors can be reported
  // by the syscall. Locking then keeps every page resident through verification.
  checked(::madvise(mapping, bytes, MADV_POPULATE_WRITE), "prefault owned local host storage");
  if (lock == NumaMemory::Lock::Pages) checked(::mlock(mapping, bytes), "page-lock compile NUMA arena (IPC_LOCK and memlock required)");
  verify_pages(mapping, bytes, node, lock);
 } catch (...) {
  ::munmap(mapping, bytes);
  throw;
 }
 return mapping;
}
}  // namespace
std::size_t host_page_size() {
 static const auto size = [] {
  const auto value = ::sysconf(_SC_PAGESIZE);
  if (value <= 0) throw std::runtime_error("host page size unavailable");
  return static_cast<std::size_t>(value);
 }();
 return size;
}
std::size_t page_rounded_bytes(std::size_t bytes) {
 const auto page = host_page_size();
 if (bytes > std::numeric_limits<std::size_t>::max() - (page - 1U)) throw std::bad_alloc();
 return ((bytes + page - 1U) / page) * page;
}
MemoryPolicy capture_memory_policy() {
 MemoryPolicy policy;
 policy.mask.resize(static_cast<std::size_t>(numa_max_possible_node()) / word_bits + 1U);
 capture_memory_policy(policy);
 return policy;
}
void capture_memory_policy(MemoryPolicy& policy) {
 if (policy.mask.empty()) throw std::invalid_argument("memory policy capture storage is not prepared");
 checked(::get_mempolicy(&policy.mode, policy.mask.data(), policy.mask.size() * word_bits, nullptr, 0), "capture thread memory policy");
}
MemoryPolicy prepare_bound_memory_policy(int node) {
 if (node < 0 || node > numa_max_possible_node()) throw std::invalid_argument("invalid host allocation NUMA node");
 MemoryPolicy policy;
 policy.mode = MPOL_BIND | MPOL_F_STATIC_NODES;
 policy.mask.resize(static_cast<std::size_t>(numa_max_possible_node()) / word_bits + 1U);
 checked(::get_mempolicy(nullptr, policy.mask.data(), policy.mask.size() * word_bits, nullptr, MPOL_F_MEMS_ALLOWED), "query permitted memory nodes");
 const auto word = static_cast<std::size_t>(node) / word_bits;
 const auto bit = 1UL << (static_cast<std::size_t>(node) % word_bits);
 if (!(policy.mask[word] & bit)) throw std::invalid_argument("required NUMA memory node is forbidden");
 std::ranges::fill(policy.mask, 0UL);
 policy.mask[word] = bit;
 return policy;
}
void restore_memory_policy(const MemoryPolicy& policy) {
 const bool empty = std::ranges::all_of(policy.mask, [](auto word) { return word == 0; });
 checked(::set_mempolicy(policy.mode, empty ? nullptr : policy.mask.data(), empty ? 0 : policy.mask.size() * word_bits), "restore thread memory policy");
}
void bind_memory_node(int node) {
 restore_memory_policy(prepare_bound_memory_policy(node));
 if (bound_memory_node(capture_memory_policy()) != node) throw std::runtime_error("worker NUMA policy verification failed");
}
int bound_memory_node(const MemoryPolicy& policy) {
 if ((policy.mode & ~(MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES)) != MPOL_BIND) return -1;
 int node = -1;
 for (std::size_t word = 0; word < policy.mask.size(); ++word)
  for (std::size_t bit = 0; bit < word_bits; ++bit)
   if (policy.mask[word] & (1UL << bit)) {
    if (node >= 0) return -1;
    node = static_cast<int>(word * word_bits + bit);
   }
 return node;
}
NumaMemory::NumaMemory(int node, Lock lock) : node_(node), lock_(lock) { (void)prepare_bound_memory_policy(node); }
NumaMemory::~NumaMemory() { reset(); }
NumaMemory::NumaMemory(NumaMemory&& other) noexcept : node_(other.node_), lock_(other.lock_), data_(std::exchange(other.data_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
NumaMemory& NumaMemory::operator=(NumaMemory&& other) noexcept {
 if (this != &other) {
  reset();
  node_ = other.node_;
  lock_ = other.lock_;
  data_ = std::exchange(other.data_, nullptr);
  bytes_ = std::exchange(other.bytes_, 0);
 }
 return *this;
}
void NumaMemory::ensure_bytes(std::size_t bytes) {
 if (bytes <= bytes_) return;
 const auto capacity = page_rounded_bytes(bytes);
 auto* next = allocate_pages(capacity, host_page_size(), node_, lock_);
 reset();
 data_ = next;
 bytes_ = capacity;
}
void NumaMemory::reset() noexcept {
 if (data_) {
  if (lock_ == Lock::Pages) ::munlock(data_, bytes_);
  ::munmap(data_, bytes_);
 }
 data_ = nullptr;
 bytes_ = 0;
}
void NumaMemory::verify() const {
 if (data_) verify_pages(data_, bytes_, node_, lock_);
}
NumaMemoryResource::NumaMemoryResource(int node) : node_(node) { (void)prepare_bound_memory_policy(node); }
void* NumaMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) { return allocate_pages(page_rounded_bytes(std::max(bytes, std::size_t{1})), alignment, node_); }
void NumaMemoryResource::do_deallocate(void* data, std::size_t bytes, std::size_t) { ::munmap(data, page_rounded_bytes(std::max(bytes, std::size_t{1}))); }
bool NumaMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept { return this == &other; }
}  // namespace mmltk::common::system
