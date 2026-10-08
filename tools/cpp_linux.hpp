#pragma once

#include "cpp_support.hpp"
#include <memory>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace mmltk::tools {

class CpuAffinity {
public:
    explicit CpuAffinity(std::size_t initial = CPU_SETSIZE, std::string_view overflow = {}) {
        for (capacity_ = initial;; capacity_ *= 2) {
            mask_.reset(CPU_ALLOC(capacity_));
            if (!mask_) throw std::bad_alloc();
            bytes_ = CPU_ALLOC_SIZE(capacity_);
            CPU_ZERO_S(bytes_, mask_.get());
            if (::sched_getaffinity(0, bytes_, mask_.get()) == 0) return;
            const int error = errno;
            if (error != EINVAL) throw std::system_error(error, std::generic_category(), "sched_getaffinity");
            if (capacity_ > static_cast<std::size_t>(INT_MAX) / 2) {
                if (!overflow.empty()) throw ToolError(std::string(overflow));
                throw std::system_error(error, std::generic_category(), "sched_getaffinity");
            }
        }
    }
    std::size_t count() const { return static_cast<std::size_t>(CPU_COUNT_S(bytes_, mask_.get())); }
    std::vector<int> cpus() const {
        std::vector<int> result;
        for (std::size_t cpu = 0; cpu < capacity_; ++cpu)
            if (CPU_ISSET_S(cpu, bytes_, mask_.get())) result.push_back(static_cast<int>(cpu));
        return result;
    }
private:
    struct Deleter { void operator()(cpu_set_t* mask) const { CPU_FREE(mask); } };
    std::unique_ptr<cpu_set_t, Deleter> mask_;
    std::size_t capacity_ = 0, bytes_ = 0;
};

template<class Error = ToolError>
struct UnixAddress {
    sockaddr_un value{};
    socklen_t length;
    UnixAddress(std::string_view path, std::string_view failure) {
        if (path.size() >= sizeof(value.sun_path)) throw Error(std::string(failure));
        value.sun_family = AF_UNIX;
        std::memcpy(value.sun_path, path.data(), path.size());
        length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    }
    const sockaddr* get() const { return reinterpret_cast<const sockaddr*>(&value); }
};

} // namespace mmltk::tools
