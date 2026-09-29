#include "src/common/system/execution_policy.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <linux/ioprio.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "src/common/system/cpu_affinity.h"
namespace mmltk::common::system {
namespace {
int current_online_cpu_count() { return static_cast<int>(std::max(0L, ::sysconf(_SC_NPROCESSORS_ONLN))); }
int get_current_nice_value() {
 errno = 0;
 const int value = ::getpriority(PRIO_PROCESS, 0);
 if (value == -1 && errno) throw std::system_error(errno, std::generic_category(), "getpriority failed");
 return value;
}
void set_nice(int value) {
 if (get_current_nice_value() == value) return;
 if (::setpriority(PRIO_PROCESS, 0, value) != 0) throw std::system_error(errno, std::generic_category(), "required worker nice priority denied");
}
void set_io(int value) {
 if (::syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, value) != 0) throw std::system_error(errno, std::generic_category(), "required worker I/O priority denied");
}
void set_scheduler(int policy, int priority) {
 sched_param parameter{};
 parameter.sched_priority = priority;
 const int error = ::pthread_setschedparam(::pthread_self(), policy, &parameter);
 if (error) throw std::system_error(error, std::generic_category(), "required normal worker scheduler denied");
}
void capture_affinity(std::vector<unsigned long>& mask) {
 if (::sched_getaffinity(0, mask.size() * sizeof(unsigned long), reinterpret_cast<cpu_set_t*>(mask.data())) != 0) throw std::system_error(errno, std::generic_category(), "capture thread affinity");
}
std::vector<unsigned long> prepare_affinity() {
 constexpr auto word_bits = sizeof(unsigned long) * 8U;
 auto count = static_cast<std::size_t>(std::max(1L, ::sysconf(_SC_NPROCESSORS_CONF)));
 for (;;) {
  std::vector<unsigned long> mask(count / word_bits + (count % word_bits != 0));
  if (::sched_getaffinity(0, mask.size() * sizeof(unsigned long), reinterpret_cast<cpu_set_t*>(mask.data())) == 0) return mask;
  if (errno != EINVAL || count > std::numeric_limits<std::size_t>::max() / 2U) throw std::system_error(errno, std::generic_category(), "prepare thread affinity");
  count *= 2U;
 }
}
void set_affinity(const std::vector<unsigned long>& mask) {
 const auto error = ::pthread_setaffinity_np(::pthread_self(), mask.size() * sizeof(unsigned long), reinterpret_cast<const cpu_set_t*>(mask.data()));
 if (error) throw std::system_error(error, std::generic_category(), "set thread affinity");
}
int current_io() {
 const auto value = ::syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
 if (value < 0) throw std::system_error(errno, std::generic_category(), "ioprio_get failed");
 return static_cast<int>(value);
}
void capture_scheduler(int& policy, int& priority) {
 sched_param parameter{};
 const auto error = ::pthread_getschedparam(::pthread_self(), &policy, &parameter);
 if (error) throw std::system_error(error, std::generic_category(), "pthread_getschedparam failed");
 priority = parameter.sched_priority;
}
}  // namespace
struct PreparedExecutionPolicy::Impl {
 explicit Impl(const ExecutionPolicyRequest& selected)
     : request(selected),
       previous_affinity(prepare_affinity()),
       desired_affinity(previous_affinity.size()),
       observed_affinity(previous_affinity.size()),
       previous_memory(capture_memory_policy()),
       observed_memory(previous_memory) {
  if (request.target_nice < -20 || request.target_nice > -10) throw std::invalid_argument("boundary worker nice priority must be -10 or better");
  constexpr auto word_bits = sizeof(unsigned long) * 8U;
  if (!request.cpu_affinity.empty()) {
   const auto cpu = request.cpu_affinity[request.worker_index % request.cpu_affinity.size()];
   if (cpu < 0 || static_cast<std::size_t>(cpu) / word_bits >= desired_affinity.size()) throw std::invalid_argument("worker affinity CPU is invalid");
   desired_affinity[static_cast<std::size_t>(cpu) / word_bits] = 1UL << (static_cast<std::size_t>(cpu) % word_bits);
  }
  if (request.numa_node >= 0) desired_memory = prepare_bound_memory_policy(request.numa_node);
 }
 ExecutionPolicyRequest request;
 std::vector<unsigned long> previous_affinity, desired_affinity, observed_affinity;
 MemoryPolicy previous_memory, desired_memory, observed_memory;
 std::array<char, 16> previous_name{};
 int previous_nice = 0, previous_io = 0, previous_scheduler = 0, previous_priority = 0;
 bool active = false;
};
PreparedExecutionPolicy::PreparedExecutionPolicy(const ExecutionPolicyRequest& request) : impl_(std::make_unique<Impl>(request)) {}
PreparedExecutionPolicy::~PreparedExecutionPolicy() {
 try {
  Restore();
 } catch (...) { std::terminate(); }
}
void PreparedExecutionPolicy::Apply() {
 auto& state = *impl_;
 if (state.active) throw std::logic_error("prepared execution policy is already borrowed");
 capture_affinity(state.previous_affinity);
 capture_memory_policy(state.previous_memory);
 const auto name_error = ::pthread_getname_np(::pthread_self(), state.previous_name.data(), state.previous_name.size());
 if (name_error) throw std::system_error(name_error, std::generic_category(), "get thread name");
 state.previous_nice = get_current_nice_value();
 state.previous_io = current_io();
 capture_scheduler(state.previous_scheduler, state.previous_priority);
 state.active = true;
 try {
  const auto& request = state.request;
  if (!request.thread_name.empty()) set_thread_name(request.thread_name);
  if (!request.cpu_affinity.empty()) {
   set_affinity(state.desired_affinity);
   capture_affinity(state.observed_affinity);
   if (state.observed_affinity != state.desired_affinity) throw std::runtime_error("worker affinity verification failed");
  }
  if (request.numa_node >= 0) {
   restore_memory_policy(state.desired_memory);
   capture_memory_policy(state.observed_memory);
   if (bound_memory_node(state.observed_memory) != request.numa_node) throw std::runtime_error("worker NUMA policy verification failed");
  }
  set_scheduler(SCHED_OTHER, 0);
  const auto target = std::min(state.previous_nice, request.target_nice);
  set_nice(target);
  if (request.storage_worker) set_io(IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0));
  int scheduler = 0, priority = 0;
  capture_scheduler(scheduler, priority);
  if (get_current_nice_value() != target || scheduler != SCHED_OTHER || (request.storage_worker && current_io() != IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0)))
   throw std::runtime_error("required worker execution policy verification failed");
 } catch (...) {
  const auto error = std::current_exception();
  Restore();
  std::rethrow_exception(error);
 }
}
void PreparedExecutionPolicy::Restore() {
 auto& state = *impl_;
 if (!state.active) return;
 capture_affinity(state.observed_affinity);
 if (state.observed_affinity != state.previous_affinity) {
  set_affinity(state.previous_affinity);
  capture_affinity(state.observed_affinity);
  if (state.observed_affinity != state.previous_affinity) throw std::runtime_error("restored thread affinity differs from request");
 }
 capture_memory_policy(state.observed_memory);
 if (state.observed_memory.mode != state.previous_memory.mode || state.observed_memory.mask != state.previous_memory.mask) restore_memory_policy(state.previous_memory);
 int scheduler = 0, priority = 0;
 capture_scheduler(scheduler, priority);
 if (scheduler != state.previous_scheduler || priority != state.previous_priority) set_scheduler(state.previous_scheduler, state.previous_priority);
 set_nice(state.previous_nice);
 if (current_io() != state.previous_io) set_io(state.previous_io);
 std::array<char, 16> name{};
 auto name_error = ::pthread_getname_np(::pthread_self(), name.data(), name.size());
 if (name_error) throw std::system_error(name_error, std::generic_category(), "get thread name");
 if (name != state.previous_name) {
  name_error = ::pthread_setname_np(::pthread_self(), state.previous_name.data());
  if (name_error) throw std::system_error(name_error, std::generic_category(), "restore thread name");
 }
 state.active = false;
}
ExecutionPolicySnapshot capture_execution_policy_snapshot() {
 ExecutionPolicySnapshot snapshot;
 snapshot.affinity = allowed_cpu_set();
 std::array<char, 16> name{};
 const auto name_error = ::pthread_getname_np(::pthread_self(), name.data(), name.size());
 if (name_error) throw std::system_error(name_error, std::generic_category(), "get thread name");
 snapshot.thread_name = name.data();
 snapshot.online_cpu_count = current_online_cpu_count();
 snapshot.nice_value = get_current_nice_value();
 snapshot.memory_policy = capture_memory_policy();
 snapshot.numa_node = bound_memory_node(snapshot.memory_policy);
 capture_scheduler(snapshot.scheduler_policy, snapshot.scheduler_priority);
 const auto io_value = current_io();
 snapshot.io_class = IOPRIO_PRIO_CLASS(io_value);
 snapshot.io_priority_data = IOPRIO_PRIO_DATA(io_value);
 return snapshot;
}
ScopedExecutionPolicy::ScopedExecutionPolicy(const ExecutionPolicyRequest& request) : owned_(std::make_unique<PreparedExecutionPolicy>(request)), policy_(owned_.get()) { policy_->Apply(); }
ScopedExecutionPolicy::ScopedExecutionPolicy(PreparedExecutionPolicy& policy) : policy_(&policy) { policy_->Apply(); }
ScopedExecutionPolicy::~ScopedExecutionPolicy() noexcept {
 try {
  Restore();
 } catch (...) { std::terminate(); }
}
void ScopedExecutionPolicy::Restore() {
 if (!policy_) return;
 policy_->Restore();
 policy_ = nullptr;
}
ExecutionPolicySnapshot apply_process_execution_policy() { return apply_worker_execution_policy(ExecutionPolicyRequest{}); }
ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest& request) {
 PreparedExecutionPolicy policy(request);
 policy.Apply();
 auto result = capture_execution_policy_snapshot();
 policy.impl_->active = false;
 return result;
}
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int clamp_worker_count_to_cpus(int requested_workers, size_t cpu_count, int reserved_cpus, int minimum_workers) {
 if (requested_workers <= 0) { throw std::invalid_argument("requested_workers must be positive"); }
 if (minimum_workers < 0) { throw std::invalid_argument("minimum_workers must be non-negative"); }
 const size_t reserved = reserved_cpus > 0 ? static_cast<size_t>(reserved_cpus) : 0U;
 const size_t minimum = minimum_workers > 0 ? static_cast<size_t>(minimum_workers) : 0U;
 size_t usable = cpu_count > reserved ? cpu_count - reserved : 0U;
 if (usable < minimum) { usable = minimum; }
 if (usable == 0) { usable = 1; }
 return std::max(1, std::min(requested_workers, static_cast<int>(usable)));
}
// NOLINTEND(bugprone-easily-swappable-parameters)
}  // namespace mmltk::common::system
