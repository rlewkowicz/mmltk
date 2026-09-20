#include "detail/benchmark_storage.h"
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
[[nodiscard]] std::uint64_t available_bytes(const std::filesystem::path& path) {
    std::filesystem::path probe = std::filesystem::absolute(path).lexically_normal();
    while (!std::filesystem::exists(probe)) {
        const std::filesystem::path parent = probe.parent_path();
        if (parent == probe || parent.empty()) { throw std::runtime_error("cannot locate an existing parent for storage preflight"); }
        probe = parent;
    }
    struct statvfs status{};
    if (::statvfs(probe.c_str(), &status) != 0) { throw common_io::errno_error("cannot inspect benchmark storage", probe.string()); }
    return common_math::checked_multiply(status.f_bavail, status.f_frsize, "available storage byte count overflow");
}
}  // namespace
std::uint64_t additional_download_bytes(const std::filesystem::path& destination, const std::uint64_t expected) {
    std::uint64_t allocated = 0;
    for (const auto& path : {destination, std::filesystem::path(destination.string()+".part")}) {
        struct stat status{};
        if (::lstat(path.c_str(), &status) != 0) {
            if (errno == ENOENT) continue;
            throw common_io::errno_error("cannot inspect retained download storage", path.string());
        }
        if (S_ISREG(status.st_mode)) allocated = std::max(allocated,
            common_math::checked_multiply(common_math::checked_cast<std::uint64_t>(status.st_blocks,"download allocation overflow"),512U,"download allocation overflow"));
    }
    return expected > allocated ? expected - allocated : 0U;
}
void require_storage(const std::filesystem::path& path, const std::uint64_t required, const char* description, const BenchmarkTraceSink& trace) {
    const std::uint64_t available = available_bytes(path);
    trace_benchmark_event(trace, "benchmark.storage.preflight", [&] {
        return nlohmann::json{{"target", description}, {"path", path.string()}, {"required_bytes", required}, {"available_bytes", available}};
    });
    if (available < required) {
        throw InsufficientBenchmarkStorage(std::string("insufficient storage for ") + description + ": requires " + std::to_string(required) + " bytes, available " +
                                 std::to_string(available));
    }
}
StorageReservationPool::StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace) : path_(std::move(path)), trace_(std::move(trace)) {}
StorageReservationPool::Reservation StorageReservationPool::reserve(const std::uint64_t required, const std::string_view description) {
    const std::lock_guard lock(mutex_);
    const std::uint64_t available = available_bytes(path_);
    if (reserved_ > available || required > available - reserved_) {
        throw InsufficientBenchmarkStorage("insufficient storage for " + std::string(description) + ": requires " + std::to_string(required) + " bytes with " +
                                 std::to_string(reserved_) + " bytes already reserved, available " + std::to_string(available));
    }
    reserved_ += required;
    trace_benchmark_event(trace_, "benchmark.storage.reserved", [&] {
        return nlohmann::json{
            {"target", description}, {"path", path_.string()}, {"required_bytes", required}, {"reserved_bytes", reserved_}, {"available_bytes", available}};
    });
    return Reservation(this, required);
}
void StorageReservationPool::release(const std::uint64_t bytes) noexcept {
    const std::lock_guard lock(mutex_);
    reserved_ = bytes <= reserved_ ? reserved_ - bytes : 0U;
}
}  // namespace mmltk::backend::data::benchmark_internal
