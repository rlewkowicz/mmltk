#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include <string>
#include <utility>
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
namespace common_math = mmltk::common::math;
namespace {
std::uint64_t allocated_bytes(const struct stat& status) {
 if (!S_ISREG(status.st_mode)) throw std::runtime_error("benchmark staging allocation is not a regular file");
 return common_math::checked_multiply(common_math::checked_cast<std::uint64_t>(status.st_blocks, "benchmark allocation overflow"), 512U, "benchmark allocation overflow");
}
std::uint64_t download_allocation(const std::filesystem::path& destination) {
 std::uint64_t allocated = 0;
 for (const auto& path : {destination, std::filesystem::path(destination.string() + ".part")}) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
   if (errno == ENOENT) continue;
   throw common_io::errno_error("cannot inspect retained download storage", path.string());
  }
  if (S_ISREG(status.st_mode)) allocated = std::max(allocated, allocated_bytes(status));
 }
 return allocated;
}
}  // namespace
std::uint64_t additional_download_bytes(const std::filesystem::path& destination, std::uint64_t expected) {
 const auto allocated = download_allocation(destination);
 return expected > allocated ? expected - allocated : 0;
}
namespace {
detail::StorageReservationPool::Observer observe_storage(const BenchmarkTraceSink& trace, bool preflight) {
 if (!trace) return {};
 return [trace, preflight](const detail::StorageReservationPool::Observation& value) {
  trace_benchmark_event(trace, preflight ? "benchmark.storage.preflight" : value.failed ? "benchmark.storage.capacity_failed" : "benchmark.storage.reserved", [&] {
   return nlohmann::json{{"target", value.description}, {"path", value.path.string()}, {"required_bytes", value.required}, {"reserved_bytes", value.outstanding}, {"available_bytes", value.available},
    {"capacity_samples", value.capacity_samples}, {"retries", value.retries}, {"duration_ns", value.duration_ns}, {"failed", value.failed}};
  });
 };
}
} // namespace
void require_storage(const std::filesystem::path& path, std::uint64_t required, const char* description, const BenchmarkTraceSink& trace) {
 detail::StorageReservationPool::require_capacity(path, required, description, observe_storage(trace, true));
}
detail::StorageReservationPool::Observer storage_observer(const BenchmarkTraceSink& trace) { return observe_storage(trace, false); }
detail::StorageReservationPool::Reservation reserve_download(detail::StorageReservationPool& storage, const std::filesystem::path& path, std::uint64_t expected, std::string_view description) {
 return storage.reserve(expected, description, download_allocation(path));
}
void reconcile_download(detail::StorageReservationPool::Reservation& reservation, const std::filesystem::path& path) { reservation.reconcile_allocated(download_allocation(path)); }
}  // namespace mmltk::backend::data::benchmark_internal
