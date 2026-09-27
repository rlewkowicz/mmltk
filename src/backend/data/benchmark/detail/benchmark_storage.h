#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <stdexcept>
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
namespace mmltk::backend::data::benchmark_internal {
class InsufficientBenchmarkStorage final : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
[[nodiscard]] std::uint64_t additional_download_bytes(const std::filesystem::path& destination, std::uint64_t expected);
void require_storage(const std::filesystem::path&, std::uint64_t, const char*, const BenchmarkTraceSink&);
class StorageReservationPool {
 struct Ledger;
 struct Destination;
public:
 class Reservation {
 public:
  Reservation() = default;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  Reservation(Reservation&&) noexcept;
  Reservation& operator=(Reservation&&) noexcept;
  ~Reservation();
  // Inspect only this changed backing; sparse logical length is never allocation.
  void reconcile(int descriptor);
  void reconcile_download(const std::filesystem::path&);
  // Restore the complete promise BEFORE removing/truncating allocated backing.
  void withdraw_allocation();
  void resize(std::uint64_t promised, std::string_view description);
  void grow(std::uint64_t promised, std::string_view description);
  void release() noexcept;
 private:
  friend class StorageReservationPool;
  Reservation(std::shared_ptr<Destination>, std::uint64_t promised, std::uint64_t allocated);
  void settle_locked(std::uint64_t allocated);
  [[nodiscard]] std::uint64_t outstanding() const noexcept;
  std::shared_ptr<Destination> destination_;
  std::uint64_t promised_ = 0, allocated_ = 0;
  std::atomic<bool> fully_allocated_{false};
 };
 // Resolves filesystem facts once; copy this view for repeated writes to it.
 StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace, StorageReservationPool* compile = nullptr);
 [[nodiscard]] Reservation reserve(std::uint64_t required, std::string_view description);
 [[nodiscard]] Reservation reserve_download(const std::filesystem::path&, std::uint64_t expected, std::string_view description);
 [[nodiscard]] std::uint64_t outstanding() const;
private:
 [[nodiscard]] Reservation reserve_backing(std::uint64_t promised, std::string_view description, const std::filesystem::path* download);
 std::shared_ptr<Destination> destination_;
};
inline constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
inline constexpr std::uint64_t kEstimatedJpegBytes = std::uint64_t{256U} * 1024U;
}  // namespace mmltk::backend::data::benchmark_internal
