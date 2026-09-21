#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <stdexcept>
#include "benchmark_cache.h"
namespace mmltk::backend::data::benchmark_internal {
class InsufficientBenchmarkStorage final : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};
// Additional allocation for an in-place/replaced download; allocated partial extents count once.
[[nodiscard]] std::uint64_t additional_download_bytes(const std::filesystem::path& destination, std::uint64_t expected);
void require_storage(const std::filesystem::path&, std::uint64_t, const char*, const BenchmarkTraceSink&);
class StorageReservationPool {
   public:
    class Reservation {
       public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&&) = delete;
        Reservation& operator=(Reservation&&) = delete;
        ~Reservation() { release(); }

       private:
        friend class StorageReservationPool;
        Reservation(StorageReservationPool* owner, const std::uint64_t bytes) : owner_(owner), bytes_(bytes) {}
        void release() noexcept {
            if (owner_ != nullptr) {
                owner_->release(bytes_);
                owner_ = nullptr;
                bytes_ = 0U;
            }
        }
        StorageReservationPool* owner_ = nullptr;
        std::uint64_t bytes_ = 0U;
    };
    StorageReservationPool(std::filesystem::path path, BenchmarkTraceSink trace);
    [[nodiscard]] Reservation reserve(const std::uint64_t required, const std::string_view description);

   private:
    void release(const std::uint64_t bytes) noexcept;
    std::filesystem::path path_;
    BenchmarkTraceSink trace_;
    std::mutex mutex_;
    std::uint64_t reserved_ = 0U;
};
inline constexpr std::uint64_t kArchiveScratchBytes = 24ULL * 1024U * 1024U * 1024U;
inline constexpr std::uint64_t kEstimatedJpegBytes = std::uint64_t{256U} * 1024U;
}  // namespace mmltk::backend::data::benchmark_internal
