#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "cpu_affinity.h"

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace mmltk {

namespace {

class CpuSetBuffer {
public:
    explicit CpuSetBuffer(size_t cpu_count)
        : cpu_count_(std::max<size_t>(cpu_count, 1)),
          bytes_(CPU_ALLOC_SIZE(cpu_count_)),
          set_(CPU_ALLOC(cpu_count_)) {
        if (set_ == nullptr) {
            throw std::bad_alloc();
        }
        CPU_ZERO_S(bytes_, set_);
    }

    ~CpuSetBuffer() {
        if (set_ != nullptr) {
            CPU_FREE(set_);
        }
    }

    CpuSetBuffer(const CpuSetBuffer&) = delete;
    CpuSetBuffer& operator=(const CpuSetBuffer&) = delete;

    [[nodiscard]] cpu_set_t* get() noexcept { return set_; }
    [[nodiscard]] const cpu_set_t* get() const noexcept { return set_; }
    [[nodiscard]] size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] size_t cpu_count() const noexcept { return cpu_count_; }

private:
    size_t cpu_count_ = 0;
    size_t bytes_ = 0;
    cpu_set_t* set_ = nullptr;
};

std::string_view trim_ascii(std::string_view value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

int parse_cpu_id(std::string_view token) {
    token = trim_ascii(token);
    if (token.empty()) {
        throw std::runtime_error("cpu affinity token must not be empty");
    }

    int value = -1;
    const char* begin = token.data();
    const char* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end || value < 0) {
        throw std::runtime_error("invalid cpu affinity token: " + std::string(token));
    }
    return value;
}

size_t configured_cpu_count() {
    const long configured = ::sysconf(_SC_NPROCESSORS_CONF);
    if (configured > 0) {
        return static_cast<size_t>(configured);
    }
    return CPU_SETSIZE;
}

} // namespace

std::vector<int> allowed_cpu_set() {
    CpuSetBuffer mask(configured_cpu_count());
    if (::sched_getaffinity(0, mask.bytes(), mask.get()) != 0) {
        throw std::system_error(errno,
                                std::generic_category(),
                                "sched_getaffinity failed");
    }

    std::vector<int> cpus;
    cpus.reserve(mask.cpu_count());
    for (size_t cpu = 0; cpu < mask.cpu_count(); ++cpu) {
        if (CPU_ISSET_S(cpu, mask.bytes(), mask.get()) != 0) {
            cpus.push_back(static_cast<int>(cpu));
        }
    }
    if (cpus.empty()) {
        throw std::runtime_error("current affinity mask does not allow any CPUs");
    }
    return cpus;
}

std::vector<int> parse_cpu_list(const std::string& spec) {
    std::vector<int> cpus;
    const std::string_view spec_view{spec};
    size_t cursor = 0;
    while (cursor <= spec_view.size()) {
        size_t next = spec.find(',', cursor);
        if (next == std::string::npos) {
            next = spec_view.size();
        }
        std::string_view token = spec_view.substr(cursor, next - cursor);
        token = trim_ascii(token);
        if (!token.empty()) {
            const size_t dash = token.find('-');
            if (dash == std::string::npos) {
                cpus.push_back(parse_cpu_id(token));
            } else {
                const int begin = parse_cpu_id(token.substr(0, dash));
                const int end = parse_cpu_id(token.substr(dash + 1));
                if (begin > end) {
                    throw std::runtime_error("cpu affinity range start exceeds end: " +
                                             std::string(token));
                }
                for (int cpu = begin; cpu <= end; ++cpu) {
                    cpus.push_back(cpu);
                }
            }
        }
        if (next == spec.size()) {
            break;
        }
        cursor = next + 1;
    }

    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    if (cpus.empty()) {
        throw std::runtime_error("cpu affinity list must not be empty");
    }
    return cpus;
}

std::vector<int> resolve_cpu_affinity(const std::string& spec) {
    std::vector<int> allowed = allowed_cpu_set();
    if (spec.empty()) {
        return allowed;
    }

    std::vector<int> requested = parse_cpu_list(spec);
    for (const int cpu : requested) {
        if (!std::binary_search(allowed.begin(), allowed.end(), cpu)) {
            throw std::runtime_error("cpu " + std::to_string(cpu) +
                                     " is outside the current allowed cpuset " +
                                     format_cpu_list(allowed));
        }
    }
    return requested;
}

std::string format_cpu_list(const std::vector<int>& cpus) {
    if (cpus.empty()) {
        return "";
    }

    std::string formatted;
    size_t index = 0;
    while (index < cpus.size()) {
        const int first = cpus[index];
        int last = first;
        while (index + 1 < cpus.size() && cpus[index + 1] == last + 1) {
            ++index;
            last = cpus[index];
        }
        if (!formatted.empty()) {
            formatted.push_back(',');
        }
        formatted += std::to_string(first);
        if (first != last) {
            formatted.push_back('-');
            formatted += std::to_string(last);
        }
        ++index;
    }
    return formatted;
}

void pin_thread_to_cpu(const std::vector<int>& cpus, size_t worker_index) {
    std::vector<int> fallback;
    const std::vector<int>& targets = cpus.empty() ? (fallback = allowed_cpu_set(), fallback) : cpus;
    if (targets.empty()) {
        throw std::runtime_error("pin_thread_to_cpu requires at least one CPU");
    }

    const int cpu = targets[worker_index % targets.size()];
    CpuSetBuffer mask(static_cast<size_t>(cpu) + 1);
    CPU_SET_S(static_cast<size_t>(cpu), mask.bytes(), mask.get());
    const int rc = ::pthread_setaffinity_np(::pthread_self(), mask.bytes(), mask.get());
    if (rc != 0) {
        throw std::system_error(rc,
                                std::generic_category(),
                                "pthread_setaffinity_np failed for cpu " +
                                    std::to_string(cpu));
    }
}

void set_thread_name(const std::string& name) {
    if (name.empty()) {
        return;
    }

    std::array<char, 16> buffer{};
    const size_t count = std::min(name.size(), buffer.size() - 1);
    std::copy_n(name.data(), count, buffer.data());
    const int rc = ::pthread_setname_np(::pthread_self(), buffer.data());
    if (rc != 0) {
        throw std::system_error(rc,
                                std::generic_category(),
                                "pthread_setname_np failed");
    }
}

} // namespace mmltk
