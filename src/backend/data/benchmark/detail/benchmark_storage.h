#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <memory>
#include <string_view>
#include <stdexcept>
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
namespace mmltk::backend::data::benchmark_internal {
class InsufficientBenchmarkStorage final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
// Additional allocation for an in-place/replaced download; allocated partial extents count once.
[[nodiscard]] std::uint64_t additional_download_bytes(const std::filesystem::path& destination, std::uint64_t expected);
void require_storage(const std::filesystem::path&, std::uint64_t, const char*, const BenchmarkTraceSink&);
class StorageReservationPool {
 struct Ledger;
 struct Entry;
public:
 class Reservation {
 public:
  Reservation() = default;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  Reservation(Reservation&&) noexcept;
  Reservation& operator=(Reservation&&) noexcept;
  ~Reservation();
  // Monotonic physical allocation attributed to this reservation. Bytes which
  // statvfs already deducted stop being outstanding promises immediately.
  void allocated(std::uint64_t bytes);
  // Follow a staging inode while it grows. Baseline allocation is excluded.
  void watch(const std::filesystem::path&, std::uint64_t baseline = 0);
  void release() noexcept;
 private:
  friend class StorageReservationPool;
  Reservation(std::shared_ptr<Ledger>, std::shared_ptr<Entry>);
  std::shared_ptr<Ledger> ledger_;
  std::shared_ptr<Entry> entry_;
 };
 // A destination view shares the compile's ledger, keyed by filesystem device.
 // Standalone utilities own their local ledger when no compile owner is given.
 StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace, StorageReservationPool* compile = nullptr);
 [[nodiscard]] Reservation reserve(std::uint64_t required, std::string_view description);
 [[nodiscard]] Reservation reserve_download(const std::filesystem::path&, std::uint64_t expected, std::string_view description);
 [[nodiscard]] std::uint64_t outstanding() const;
private:
 std::filesystem::path path_;
 BenchmarkTraceSink trace_;
 std::shared_ptr<Ledger> ledger_;
 std::uint64_t device_ = 0;
};
inline constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
inline constexpr std::uint64_t kEstimatedJpegBytes = std::uint64_t{256U} * 1024U;
}  // namespace mmltk::backend::data::benchmark_internal
