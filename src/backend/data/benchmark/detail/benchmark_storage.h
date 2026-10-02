#pragma once
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/detail/storage_reservation.h"
#include <cstdint>
#include <filesystem>
#include <string_view>
namespace mmltk::backend::data::benchmark_internal {
[[nodiscard]] std::uint64_t additional_download_bytes(const std::filesystem::path&, std::uint64_t expected);
void require_storage(const std::filesystem::path&, std::uint64_t, const char*, const BenchmarkTraceSink&);
[[nodiscard]] detail::StorageReservationPool::Observer storage_observer(const BenchmarkTraceSink&);
[[nodiscard]] detail::StorageReservationPool::Reservation reserve_download(detail::StorageReservationPool&, const std::filesystem::path&, std::uint64_t, std::string_view);
void reconcile_download(detail::StorageReservationPool::Reservation&, const std::filesystem::path&);
inline constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
inline constexpr std::uint64_t kEstimatedJpegBytes = std::uint64_t{256U} * 1024U;
}  // namespace mmltk::backend::data::benchmark_internal
