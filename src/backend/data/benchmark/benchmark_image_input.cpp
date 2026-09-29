#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
namespace {
namespace io = mmltk::common::io;
using Cancellation = mmltk::common::concurrency::CancellationObservation;
std::array<std::uint64_t, 5> identity(const struct stat& value) {
 // Rename/unlink can change ctime; neither changes the immutable payload.
 return {
  static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino), static_cast<std::uint64_t>(value.st_size), static_cast<std::uint64_t>(value.st_mtim.tv_sec),
  static_cast<std::uint64_t>(value.st_mtim.tv_nsec)
 };
}
std::shared_ptr<io::MappedByteRegion> map_image(int descriptor, std::size_t bytes) {
 auto mapping = std::make_shared<io::MappedByteRegion>();
 void* address = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, descriptor, 0);
 if (address == MAP_FAILED) throw io::errno_error("cannot map cached benchmark image");
 mapping->adopt(address, bytes);
 return mapping;
}
}  // namespace
std::filesystem::path cached_image_path(const std::filesystem::path& root, const std::uint64_t image_id) {
 std::array<char, 24> relative{};
 const std::size_t length = format_cached_image_relative_path(image_id, relative);
 return root / std::string(relative.data(), length);
}
std::size_t format_cached_image_relative_path(const std::uint64_t image_id, const std::span<char> output) {
 constexpr std::size_t kPathCharacters = 23U;
 constexpr std::size_t kRequiredBytes = kPathCharacters + 1U;
 if (output.size() < kRequiredBytes) { throw std::runtime_error("cached benchmark image path buffer is too small"); }
 const int length = std::snprintf(output.data(), output.size(), "%02llx/%016llx.jpg", static_cast<unsigned long long>(image_id & 0xFFU), static_cast<unsigned long long>(image_id));
 if (length != static_cast<int>(kPathCharacters)) { throw std::runtime_error("cannot format cached benchmark image ID"); }
 return static_cast<std::size_t>(length);
}
BenchmarkImageReadError::BenchmarkImageReadError(std::uint16_t source, std::uint64_t id, std::string detail)
    : std::runtime_error("benchmark cached image " + std::to_string(id) + " cannot be read: " + std::move(detail)), source_index_(source), source_image_id_(id) {}
std::uint16_t BenchmarkImageReadError::source_index() const noexcept { return source_index_; }
std::uint64_t BenchmarkImageReadError::source_image_id() const noexcept { return source_image_id_; }
BenchmarkEncodedImage::BenchmarkEncodedImage(
 BenchmarkAllowance allowance, std::shared_ptr<const void> backing, std::span<const std::uint8_t> encoded, BenchmarkImageHeader header, Identity stamp, Storage storage, bool charged, Ptr file)
    : allowance_(std::move(allowance)),
      backing_(std::move(backing)),
      encoded_(encoded),
      header_(header),
      identity_(stamp),
      storage_(storage),
      charged_bytes_(charged),
      file_backing_(std::move(file)) {}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::pooled(BenchmarkAllowance allowance, std::shared_ptr<const void> backing, std::span<const std::uint8_t> encoded, Ptr file) {
 if (!backing || !file || encoded.empty() || encoded.size() != file->size()) throw std::invalid_argument("invalid pooled benchmark image custody");
 const auto header = file->header_;
 const auto stamp = file->identity_;
 return Ptr(new BenchmarkEncodedImage(std::move(allowance), std::move(backing), encoded, header, stamp, Storage::Pooled, true, std::move(file)));
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::header_only() const { return Ptr(new BenchmarkEncodedImage({}, {}, {}, header_, identity_, Storage::HeaderOnly, false)); }
BenchmarkEncodedImage::Opened::Opened(io::FileHandle file, std::uint64_t id, Identity stamp, BenchmarkAllowance controls, Ptr admitted)
    : controls_(std::move(controls)), file_(std::move(file)), image_id_(id), identity_(stamp), admitted_(std::move(admitted)) {}
BenchmarkEncodedImage::Opened::~Opened() = default;
std::unique_ptr<BenchmarkEncodedImage::Opened> BenchmarkEncodedImage::inspect(io::FileHandle file, std::uint64_t id, BenchmarkAllowance allowance, Ptr admitted, std::uint64_t maximum_bytes) {
 struct stat status{};
 if (::fstat(file.get(), &status) != 0) throw io::errno_error("cannot inspect cached benchmark image");
 if (!S_ISREG(status.st_mode) || status.st_size <= 0) return {};
 const auto bytes = mmltk::common::math::checked_cast<std::size_t>(status.st_size, "cached image extent overflow");
 if (bytes > maximum_bytes) throw InvalidImageError("cached benchmark image has an invalid size");
 return std::unique_ptr<Opened>(new Opened(std::move(file), id, identity(status), std::move(allowance), std::move(admitted)));
}
std::unique_ptr<BenchmarkEncodedImage::Opened> BenchmarkEncodedImage::open_deferred(
 int directory, std::uint64_t id, Cancellation cancellation, BenchmarkAllowance controls, Ptr admitted, std::uint64_t maximum_bytes) {
 throw_if_benchmark_cancelled(cancellation);
 std::array<char, 24> relative{};
 (void)format_cached_image_relative_path(id, relative);
 const int descriptor = ::openat(directory, relative.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
 if (descriptor < 0) {
  if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) return {};
  throw io::errno_error("cannot open cached benchmark image", relative.data());
 }
 return inspect(io::FileHandle(descriptor), id, std::move(controls), std::move(admitted), maximum_bytes);
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::open(int directory, std::uint64_t id, const CachedImageValidator& validator, Cancellation cancellation, BenchmarkCompilePipeline* execution,
 BenchmarkAllowance allowance, Ptr admitted, std::uint64_t maximum_bytes) {
 auto opened = open_deferred(directory, id, cancellation, allowance, std::move(admitted), maximum_bytes);
 return opened ? read_opened(*opened, validator, cancellation, execution, std::move(allowance), false) : Ptr{};
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::open(io::FileHandle file, std::uint64_t id, const CachedImageValidator& validator, Cancellation cancellation, BenchmarkCompilePipeline* execution,
 BenchmarkAllowance allowance, Ptr admitted, std::uint64_t maximum_bytes) {
 auto opened = inspect(std::move(file), id, allowance, std::move(admitted), maximum_bytes);
 return opened ? read_opened(*opened, validator, cancellation, execution, std::move(allowance), false) : Ptr{};
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::Opened::try_read(BenchmarkCompilePipeline* execution, Cancellation cancellation) {
 try {
  throw_if_benchmark_cancelled(cancellation);
  if (file_.get() < 0) throw std::logic_error("cached benchmark image was already consumed");
  BenchmarkAllowance workspace, mapping;
  if (execution) {
   const std::uint64_t header_bytes = admitted_ && admitted_->identity_ == identity_ ? 0 : 64U << 10;
   const auto bytes = mmltk::common::math::checked_add(identity_[2], header_bytes, "cached image workspace overflow");
   auto complete = execution->try_reserve({bytes, 0}, controls_);
   if (!complete) {
    if (!wait_) wait_ = execution->defer_resources();
    return {};
   }
   wait_.reset();
   workspace = std::move(*complete);
   mapping = workspace.split_storage(identity_[2]);
  }
  auto result = read_opened(*this, {}, cancellation, execution, std::move(mapping), execution != nullptr);
  // The decoder lives only inside read_opened's CPU callback. Neither its
  // scratch nor this file descriptor is retained by the completed mapping.
  file_ = {};
  workspace.retire_workspace();
  return result;
 } catch (...) {
  file_ = {};
  wait_.reset();
  throw;
 }
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::read_opened(
 const Opened& opened, const CachedImageValidator& validator, Cancellation cancellation, BenchmarkCompilePipeline* execution, BenchmarkAllowance allowance, bool charged) {
 const auto bytes = static_cast<std::size_t>(opened.identity_[2]);
 auto mapping = map_image(opened.file_.get(), bytes);
 const std::span<const std::uint8_t> encoded{static_cast<const std::uint8_t*>(mapping->address()), bytes};
 BenchmarkImageHeader header;
 if (opened.admitted_ && opened.admitted_->identity_ == opened.identity_)
  header = opened.admitted_->header_;
 else {
  try {
   const auto read = [&](std::size_t) { header = validator ? validator(opened.image_id_, encoded) : BenchmarkImageDecoder{}.read_header(encoded); };
   if (execution)
    execution->run(BenchmarkStage::Header, {}, read, allowance);
   else
    read(0);
  } catch (const std::bad_alloc&) { throw; } catch (const std::exception& error) {
   throw_if_benchmark_cancelled(cancellation);
   if (is_benchmark_capacity_failure(error)) throw;
   throw InvalidImageError(error.what());
  }
 }
 throw_if_benchmark_cancelled(cancellation);
 return Ptr(new BenchmarkEncodedImage(std::move(allowance), mapping, encoded, header, opened.identity_, Storage::Mapped, charged));
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::publish(const std::filesystem::path& path, std::span<const std::uint8_t> encoded, Cancellation cancellation, StorageReservationPool& destination,
 std::optional<BenchmarkImageHeader> header, BenchmarkAllowance allowance, bool retain_mapping, int directory, std::string_view relative) {
 if (encoded.empty()) throw std::runtime_error("cannot cache an empty benchmark image");
 const auto target = directory >= 0 ? std::filesystem::path(relative) : path;
 auto staging = BenchmarkStagedArtifact::create(destination, target, encoded.size(), "cached image staging", ".tmp.XXXXXX", 0600, directory);
 staging.file().pwrite_all(encoded.data(), encoded.size(), 0);
 Ptr result;
 if (header) {
  struct stat status{};
  if (::fstat(staging.file().get(), &status) != 0) throw io::errno_error("cannot inspect published benchmark image");
  auto mapping = retain_mapping ? map_image(staging.file().get(), encoded.size()) : nullptr;
  const auto view = mapping ? std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(mapping->address()), encoded.size()) : std::span<const std::uint8_t>{};
  result = Ptr(new BenchmarkEncodedImage(std::move(allowance), mapping, view, *header, identity(status), mapping ? Storage::Mapped : Storage::HeaderOnly, false));
 }
 staging.publish(target, cancellation, BenchmarkStagedArtifact::Publication::Rename);
 return result;
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::publish(int directory, std::uint64_t id, std::span<const std::uint8_t> encoded, BenchmarkImageHeader header, Cancellation cancellation,
 StorageReservationPool& storage, BenchmarkCompilePipeline* execution, BenchmarkAllowance input, bool retain_mapping) {
 auto mapping = execution && retain_mapping ? execution->try_reserve(BenchmarkResources::handles(1), input) : std::optional<BenchmarkAllowance>{};
 std::array<char, 24> relative{};
 const auto length = format_cached_image_relative_path(id, relative);
 return publish(
  {}, encoded, cancellation, storage, header, mapping.value_or(std::move(input)), retain_mapping && (!execution || mapping.has_value()), directory, std::string_view(relative.data(), length));
}
BenchmarkEncodedImage::Ptr BenchmarkEncodedImage::publish(const std::filesystem::path& path, std::span<const std::uint8_t> encoded, Cancellation cancellation, StorageReservationPool* storage,
 std::optional<BenchmarkImageHeader> header, BenchmarkAllowance allowance, bool retain_mapping, int directory, std::string_view relative) {
 StorageReservationPool destination(path, {}, storage);
 return BenchmarkEncodedImage::publish(path, encoded, cancellation, destination, header, std::move(allowance), retain_mapping, directory, relative);
}
}  // namespace mmltk::backend::data::benchmark_internal
