#pragma once
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/common/concurrency/cancellation_observation.h"
#include "src/common/io/file_memory.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
class BenchmarkResourceWait;
class StorageReservationPool;
using CachedImageValidator = std::function<BenchmarkImageHeader(std::uint64_t, std::span<const std::uint8_t>)>;
class BenchmarkImageReadError final : public std::runtime_error {
public:
 BenchmarkImageReadError(std::uint16_t, std::uint64_t, std::string);
 [[nodiscard]] std::uint16_t source_index() const noexcept;
 [[nodiscard]] std::uint64_t source_image_id() const noexcept;

private:
 std::uint16_t source_index_;
 std::uint64_t source_image_id_;
};
// A header is inseparable from the immutable bytes that admitted it. A compact
// file fact carries no mapping/descriptor; reopening checks its inode identity
// before reusing the header. Source publications separately retain mutation custody.
class BenchmarkEncodedImage final {
public:
 using Ptr = std::shared_ptr<const BenchmarkEncodedImage>;
 enum class Storage : std::uint8_t { Pooled, Mapped, HeaderOnly };
 class Opened;
 [[nodiscard]] static Ptr pooled(BenchmarkAllowance, std::shared_ptr<const void>, std::span<const std::uint8_t>, Ptr published);
 [[nodiscard]] static Ptr open(int directory, std::uint64_t, const CachedImageValidator&, mmltk::common::concurrency::CancellationObservation, BenchmarkCompilePipeline* = nullptr,
  BenchmarkAllowance = {}, Ptr admitted = {}, std::uint64_t maximum_bytes = std::numeric_limits<std::uint64_t>::max());
 [[nodiscard]] static Ptr open(mmltk::common::io::FileHandle, std::uint64_t, const CachedImageValidator&, mmltk::common::concurrency::CancellationObservation, BenchmarkCompilePipeline*,
  BenchmarkAllowance, Ptr admitted, std::uint64_t maximum_bytes);
 // Inspect once before nonblocking mapped-input admission. A null record is a
 // cache miss; an opened record keeps that exact inode through deferral.
 [[nodiscard]] static std::unique_ptr<Opened> open_deferred(int directory, std::uint64_t, mmltk::common::concurrency::CancellationObservation, BenchmarkAllowance controls, Ptr admitted = {},
  std::uint64_t maximum_bytes = std::numeric_limits<std::uint64_t>::max());
 static Ptr publish(const std::filesystem::path&, std::span<const std::uint8_t>, mmltk::common::concurrency::CancellationObservation, StorageReservationPool&, std::optional<BenchmarkImageHeader> = {},
  BenchmarkAllowance = {}, bool retain_mapping = true, int directory = -1, std::string_view relative = {});
 static Ptr publish(const std::filesystem::path&, std::span<const std::uint8_t>, mmltk::common::concurrency::CancellationObservation, StorageReservationPool* = nullptr,
  std::optional<BenchmarkImageHeader> = {}, BenchmarkAllowance = {}, bool retain_mapping = true, int directory = -1, std::string_view relative = {});
 // Shared source-worker publication choice, including nonblocking FD pressure.
 static Ptr publish(int directory, std::uint64_t, std::span<const std::uint8_t>, BenchmarkImageHeader, mmltk::common::concurrency::CancellationObservation, StorageReservationPool&,
  BenchmarkCompilePipeline*, BenchmarkAllowance input = {}, bool retain_mapping = true);
 [[nodiscard]] Ptr header_only() const;
 [[nodiscard]] const BenchmarkImageHeader& header() const noexcept { return header_; }
 [[nodiscard]] std::span<const std::uint8_t> encoded() const noexcept { return encoded_; }
 [[nodiscard]] std::size_t size() const noexcept { return identity_[2]; }
 [[nodiscard]] const BenchmarkAllowance& allowance() const noexcept { return allowance_; }
 [[nodiscard]] const std::shared_ptr<const void>& backing() const noexcept { return backing_; }
 [[nodiscard]] const Ptr& file_backing() const noexcept { return file_backing_; }
 [[nodiscard]] Storage storage() const noexcept { return storage_; }
 [[nodiscard]] bool charged_bytes() const noexcept { return charged_bytes_; }

private:
 using Identity = std::array<std::uint64_t, 5>;
 BenchmarkEncodedImage(BenchmarkAllowance, std::shared_ptr<const void>, std::span<const std::uint8_t>, BenchmarkImageHeader, Identity, Storage, bool, Ptr = {});
 [[nodiscard]] static std::unique_ptr<Opened> inspect(mmltk::common::io::FileHandle, std::uint64_t, BenchmarkAllowance, Ptr, std::uint64_t);
 [[nodiscard]] static Ptr read_opened(const Opened&, const CachedImageValidator&, mmltk::common::concurrency::CancellationObservation, BenchmarkCompilePipeline*, BenchmarkAllowance, bool);
 BenchmarkAllowance allowance_;
 std::shared_ptr<const void> backing_;
 std::span<const std::uint8_t> encoded_;
 BenchmarkImageHeader header_;
 Identity identity_;
 Storage storage_;
 bool charged_bytes_;
 Ptr file_backing_;
};
// Only descriptor/control custody is retained while admission is unavailable.
// Parser scratch is constructed after the complete byte grant and destroyed
// before returning its share; mapped bytes stay charged through their last reader.
class BenchmarkEncodedImage::Opened final {
public:
 // Null means byte admission is deferred; retry this record on an admission
 // event. Success consumes the descriptor, and errors retire pending demand.
 [[nodiscard]] Ptr try_read(BenchmarkCompilePipeline*, mmltk::common::concurrency::CancellationObservation);
 ~Opened();
 Opened(const Opened&) = delete;
 Opened& operator=(const Opened&) = delete;

private:
 friend class BenchmarkEncodedImage;
 Opened(mmltk::common::io::FileHandle, std::uint64_t, Identity, BenchmarkAllowance, Ptr);
 BenchmarkAllowance controls_;
 std::unique_ptr<BenchmarkResourceWait> wait_;
 mmltk::common::io::FileHandle file_;
 std::uint64_t image_id_;
 Identity identity_;
 Ptr admitted_;
};
[[nodiscard]] std::filesystem::path cached_image_path(const std::filesystem::path&, std::uint64_t);
[[nodiscard]] std::size_t format_cached_image_relative_path(std::uint64_t, std::span<char>);
}  // namespace mmltk::backend::data::benchmark_internal
