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
    if (::setpriority(PRIO_PROCESS, 0, value) != 0)
        throw std::system_error(errno, std::generic_category(), "required worker nice priority denied");
}
void set_io(int value) {
    if (::syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, value) != 0)
        throw std::system_error(errno, std::generic_category(), "required worker I/O priority denied");
}
void set_scheduler(int policy, int priority) {
    sched_param parameter{};
    parameter.sched_priority = priority;
    const int error = ::pthread_setschedparam(::pthread_self(), policy, &parameter);
    if (error) throw std::system_error(error, std::generic_category(), "required normal worker scheduler denied");
}
void verify_pinned_cpu(const std::vector<int>& cpus, size_t worker_index) {
    if (cpus.empty()) { return; }
    const std::vector<int> effective = allowed_cpu_set();
    const int expected_cpu = cpus[worker_index % cpus.size()];
    if (effective.size() != 1 || effective.front() != expected_cpu) {
        throw std::runtime_error("worker affinity verification failed: expected cpu " + std::to_string(expected_cpu) + ", got " +
                                 format_cpu_list(effective));
    }
}

ExecutionPolicySnapshot apply_execution_policy(const ExecutionPolicyRequest& request) {
    if (request.target_nice < -20 || request.target_nice > -10)
        throw std::invalid_argument("boundary worker nice priority must be -10 or better");
    if (!request.thread_name.empty()) { set_thread_name(request.thread_name); }
    if (!request.cpu_affinity.empty()) {
        pin_thread_to_cpu(request.cpu_affinity, request.worker_index);
        verify_pinned_cpu(request.cpu_affinity, request.worker_index);
    }
    if (request.numa_node >= 0) bind_memory_node(request.numa_node);
    set_scheduler(SCHED_OTHER, 0);
    const int target = std::min(get_current_nice_value(), request.target_nice);
    set_nice(target);
    if (request.storage_worker) set_io(IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 0));
    auto result = capture_execution_policy_snapshot();
    if (result.nice_value != target || result.scheduler_policy != SCHED_OTHER ||
        (request.storage_worker && (result.io_class != IOPRIO_CLASS_BE || result.io_priority_data != 0)) ||
        (request.numa_node >= 0 && result.numa_node != request.numa_node))
        throw std::runtime_error("required worker execution policy verification failed");
    return result;
}

}  // namespace

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

    sched_param param{};
    int policy = 0;
    const int sched_rc = ::pthread_getschedparam(::pthread_self(), &policy, &param);
    if (sched_rc != 0) { throw std::system_error(sched_rc, std::generic_category(), "pthread_getschedparam failed"); }
    snapshot.scheduler_policy = policy;
    snapshot.scheduler_priority = param.sched_priority;

    errno = 0;
    const long io_value = ::syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
    if (io_value < 0 && errno != 0) { throw std::system_error(errno, std::generic_category(), "ioprio_get failed"); }
    snapshot.io_class = IOPRIO_PRIO_CLASS(static_cast<int>(io_value));
    snapshot.io_priority_data = IOPRIO_PRIO_DATA(static_cast<int>(io_value));
    return snapshot;
}

ScopedExecutionPolicy::ScopedExecutionPolicy(const ExecutionPolicyRequest& request) : previous_(capture_execution_policy_snapshot()) {
    try {
        (void)apply_execution_policy(request);
    } catch (...) {
        const auto error = std::current_exception();
        Restore();
        std::rethrow_exception(error);
    }
}
ScopedExecutionPolicy::~ScopedExecutionPolicy() noexcept {
    if (active_) {
        try {
            Restore();
        } catch (...) { std::terminate(); }
    }
}
void ScopedExecutionPolicy::Restore() {
    if (!active_) return;
    const auto current = capture_execution_policy_snapshot();
    if (current.affinity != previous_.affinity) set_thread_affinity(previous_.affinity);
    if (current.memory_policy.mode != previous_.memory_policy.mode || current.memory_policy.mask != previous_.memory_policy.mask)
        restore_memory_policy(previous_.memory_policy);
    if (current.scheduler_policy != previous_.scheduler_policy || current.scheduler_priority != previous_.scheduler_priority)
        set_scheduler(previous_.scheduler_policy, previous_.scheduler_priority);
    set_nice(previous_.nice_value);
    if (current.io_class != previous_.io_class || current.io_priority_data != previous_.io_priority_data)
        set_io(IOPRIO_PRIO_VALUE(previous_.io_class, previous_.io_priority_data));
    if (current.thread_name != previous_.thread_name) set_thread_name(previous_.thread_name);
    active_ = false;
}
ExecutionPolicySnapshot apply_process_execution_policy() { return apply_execution_policy(ExecutionPolicyRequest{}); }

ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest& request) { return apply_execution_policy(request); }

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
