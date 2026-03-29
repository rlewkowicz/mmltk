#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "execution_policy.h"

#include "cpu_affinity.h"

#include <algorithm>
#include <linux/ioprio.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fastloader {

namespace {

constexpr int kTargetNice = -20;

bool is_permission_error(int error_code) {
    return error_code == EACCES || error_code == EPERM;
}

int current_online_cpu_count() {
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        return static_cast<int>(online);
    }
    return 0;
}

int get_current_nice_value() {
    errno = 0;
    const int nice_value = ::getpriority(PRIO_PROCESS, 0);
    if (nice_value == -1 && errno != 0) {
        throw std::system_error(errno, std::generic_category(), "getpriority failed");
    }
    return nice_value;
}

void maximize_current_thread_nice() {
    const int current = get_current_nice_value();
    for (int target = kTargetNice; target < current; ++target) {
        errno = 0;
        if (::setpriority(PRIO_PROCESS, 0, target) == 0) {
            return;
        }
        if (!is_permission_error(errno)) {
            throw std::system_error(errno, std::generic_category(), "setpriority failed");
        }
    }
}

void maximize_current_thread_scheduler(bool prefer_realtime_scheduler) {
    if (!prefer_realtime_scheduler) {
        return;
    }

    const int max_priority = ::sched_get_priority_max(SCHED_RR);
    if (max_priority < 0) {
        throw std::system_error(errno, std::generic_category(), "sched_get_priority_max failed");
    }

    for (int priority = max_priority; priority >= 1; --priority) {
        sched_param param{};
        param.sched_priority = priority;
        const int rc = ::pthread_setschedparam(::pthread_self(), SCHED_RR, &param);
        if (rc == 0) {
            return;
        }
        if (!is_permission_error(rc)) {
            throw std::system_error(rc,
                                    std::generic_category(),
                                    "pthread_setschedparam failed");
        }
    }
}

bool try_set_current_thread_ioprio(int io_class, int io_data) {
    errno = 0;
    if (::syscall(SYS_ioprio_set,
                  IOPRIO_WHO_PROCESS,
                  0,
                  IOPRIO_PRIO_VALUE(io_class, io_data)) == 0) {
        return true;
    }
    if (!is_permission_error(errno)) {
        throw std::system_error(errno, std::generic_category(), "ioprio_set failed");
    }
    return false;
}

void maximize_current_thread_ioprio(bool prefer_realtime_io) {
    if (prefer_realtime_io && try_set_current_thread_ioprio(IOPRIO_CLASS_RT, 0)) {
        return;
    }
    for (int priority = 0; priority <= 7; ++priority) {
        if (try_set_current_thread_ioprio(IOPRIO_CLASS_BE, priority)) {
            return;
        }
    }
}

const char* scheduler_policy_name(int policy) {
    switch (policy) {
    case SCHED_OTHER:
        return "SCHED_OTHER";
    case SCHED_FIFO:
        return "SCHED_FIFO";
    case SCHED_RR:
        return "SCHED_RR";
#ifdef SCHED_BATCH
    case SCHED_BATCH:
        return "SCHED_BATCH";
#endif
#ifdef SCHED_IDLE
    case SCHED_IDLE:
        return "SCHED_IDLE";
#endif
    default:
        return "SCHED_UNKNOWN";
    }
}

const char* ioprio_class_name(int io_class) {
    switch (io_class) {
    case IOPRIO_CLASS_NONE:
        return "none";
    case IOPRIO_CLASS_RT:
        return "rt";
    case IOPRIO_CLASS_BE:
        return "best-effort";
    case IOPRIO_CLASS_IDLE:
        return "idle";
    default:
        return "unknown";
    }
}

void verify_pinned_cpu(const std::vector<int>& cpus, size_t worker_index) {
    if (cpus.empty()) {
        return;
    }
    const std::vector<int> effective = allowed_cpu_set();
    const int expected_cpu = cpus[worker_index % cpus.size()];
    if (effective.size() != 1 || effective.front() != expected_cpu) {
        throw std::runtime_error("worker affinity verification failed: expected cpu " +
                                 std::to_string(expected_cpu) + ", got " +
                                 format_cpu_list(effective));
    }
}

ExecutionPolicySnapshot apply_execution_policy(const ExecutionPolicyRequest& request) {
    if (!request.thread_name.empty()) {
        set_thread_name(request.thread_name);
    }
    if (!request.cpu_affinity.empty()) {
        pin_thread_to_cpu(request.cpu_affinity, request.worker_index);
        verify_pinned_cpu(request.cpu_affinity, request.worker_index);
    }
    maximize_current_thread_scheduler(request.prefer_realtime_scheduler);
    maximize_current_thread_nice();
    maximize_current_thread_ioprio(request.prefer_realtime_io);
    return capture_execution_policy_snapshot();
}

} // namespace

ExecutionPolicySnapshot capture_execution_policy_snapshot() {
    ExecutionPolicySnapshot snapshot;
    snapshot.affinity = allowed_cpu_set();
    snapshot.online_cpu_count = current_online_cpu_count();
    snapshot.nice_value = get_current_nice_value();

    sched_param param{};
    int policy = 0;
    const int sched_rc = ::pthread_getschedparam(::pthread_self(), &policy, &param);
    if (sched_rc != 0) {
        throw std::system_error(sched_rc,
                                std::generic_category(),
                                "pthread_getschedparam failed");
    }
    snapshot.scheduler_policy = policy;
    snapshot.scheduler_priority = param.sched_priority;

    errno = 0;
    const long io_value = ::syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
    if (io_value < 0 && errno != 0) {
        throw std::system_error(errno, std::generic_category(), "ioprio_get failed");
    }
    snapshot.io_class = IOPRIO_PRIO_CLASS(static_cast<int>(io_value));
    snapshot.io_priority_data = IOPRIO_PRIO_DATA(static_cast<int>(io_value));
    return snapshot;
}

ExecutionPolicySnapshot apply_process_execution_policy() {
    return apply_execution_policy(ExecutionPolicyRequest{});
}

ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest& request) {
    return apply_execution_policy(request);
}

int clamp_worker_count_to_cpus(int requested_workers,
                               size_t cpu_count,
                               int reserved_cpus,
                               int minimum_workers) {
    if (requested_workers <= 0) {
        throw std::invalid_argument("requested_workers must be positive");
    }
    if (minimum_workers < 0) {
        throw std::invalid_argument("minimum_workers must be non-negative");
    }

    const size_t reserved = reserved_cpus > 0 ? static_cast<size_t>(reserved_cpus) : 0U;
    const size_t minimum = minimum_workers > 0 ? static_cast<size_t>(minimum_workers) : 0U;
    size_t usable = cpu_count > reserved ? cpu_count - reserved : 0U;
    if (usable < minimum) {
        usable = minimum;
    }
    if (usable == 0) {
        usable = 1;
    }
    return std::max(1, std::min(requested_workers, static_cast<int>(usable)));
}

void log_worker_budget_clamp(const char* subsystem,
                             int requested_workers,
                             int applied_workers,
                             const std::vector<int>& cpus,
                             int reserved_cpus,
                             int minimum_workers) {
    if (requested_workers == applied_workers) {
        if (reserved_cpus > 0 && cpus.size() <= static_cast<size_t>(reserved_cpus)) {
            std::fprintf(stderr,
                         "[exec] %s cpuset=%s is too small to reserve %d helper cpu(s); helper threads will overlap\n",
                         subsystem,
                         format_cpu_list(cpus).c_str(),
                         reserved_cpus);
        }
        return;
    }

    std::fprintf(stderr,
                 "[exec] %s workers clamped %d->%d for cpuset=%s (reserved=%d minimum=%d)\n",
                 subsystem,
                 requested_workers,
                 applied_workers,
                 format_cpu_list(cpus).c_str(),
                 reserved_cpus,
                 minimum_workers);
}

void log_process_execution_policy(const char* process_label,
                                  const ExecutionPolicySnapshot& snapshot,
                                  bool expect_realtime_scheduler,
                                  bool expect_realtime_io) {
    const bool cpuset_restricted =
        snapshot.online_cpu_count > 0 &&
        static_cast<int>(snapshot.affinity.size()) < snapshot.online_cpu_count;
    const bool scheduler_degraded =
        expect_realtime_scheduler && snapshot.scheduler_policy != SCHED_RR;
    const bool nice_degraded = snapshot.nice_value != kTargetNice;
    const bool io_degraded =
        expect_realtime_io &&
        !(snapshot.io_class == IOPRIO_CLASS_RT && snapshot.io_priority_data == 0);

    if (!cpuset_restricted && !scheduler_degraded && !nice_degraded && !io_degraded) {
        return;
    }

    std::fprintf(stderr,
                 "[exec] %s cpuset=%s online=%d scheduler=%s/%d nice=%d io=%s/%d\n",
                 process_label,
                 format_cpu_list(snapshot.affinity).c_str(),
                 snapshot.online_cpu_count,
                 scheduler_policy_name(snapshot.scheduler_policy),
                 snapshot.scheduler_priority,
                 snapshot.nice_value,
                 ioprio_class_name(snapshot.io_class),
                 snapshot.io_priority_data);
}

} // namespace fastloader
