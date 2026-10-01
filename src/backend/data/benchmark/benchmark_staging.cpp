#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include <sys/mman.h>
#include <stdexcept>
#include <cstring>
#include <sys/stat.h>
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
BenchmarkMappedArtifact::BenchmarkMappedArtifact(detail::StorageReservationPool& storage, const std::filesystem::path& path, std::size_t bytes, std::string_view description) : bytes_(bytes) {
 if (!bytes) return;
 staged_ = detail::StagedArtifact::create(storage, path, bytes, description);
 staged_.resize(bytes, description);
 struct stat identity{};
 if (::fstat(staged_.file().get(), &identity) != 0) throw common_io::errno_error("cannot inspect benchmark staged artifact", staged_.path().string());
 device_ = identity.st_dev;
 inode_ = identity.st_ino;
}
void BenchmarkMappedArtifact::write(const void* source, std::size_t bytes, std::size_t offset) {
 if (mapped_) throw std::logic_error("benchmark mapped artifact is already mapped");
 if (offset > bytes_ || bytes > bytes_ - offset) throw std::out_of_range("benchmark mapped write exceeds fixed extent");
 staged_.file().pwrite_all(source, bytes, offset);
}
void* BenchmarkMappedArtifact::map(int protection, int flags) {
 if (mapped_) throw std::logic_error("benchmark mapped artifact is already mapped");
 if (bytes_) {
  void* address = ::mmap(nullptr, bytes_, protection, flags, staged_.file().get(), 0);
  if (address == MAP_FAILED) throw common_io::errno_error("cannot map benchmark staged artifact", staged_.path().string());
  region_.adopt(address, bytes_);
  staged_.close();
 }
 writable_ = (protection & PROT_WRITE) != 0;
 mapped_ = true;
 return region_.address();
}
const void* BenchmarkMappedArtifact::map_readonly() { return map(PROT_READ, MAP_PRIVATE); }
void* BenchmarkMappedArtifact::map_writable() { return map(PROT_READ | PROT_WRITE, MAP_SHARED); }
bool BenchmarkMappedArtifact::can_publish(const std::filesystem::path& destination) const {
 if (!mapped_ || !writable_ || published_ || !bytes_) return false;
 struct stat target{};
 const auto directory = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
 if (::stat(directory.c_str(), &target) != 0) throw common_io::errno_error("cannot inspect normalized cache destination", directory.string());
 return target.st_dev == device_;
}
void BenchmarkMappedArtifact::seal_prefix(std::span<const std::byte> prefix) {
 if (!mapped_ || !writable_ || published_ || prefix.size() > bytes_) throw std::logic_error("benchmark mapped artifact cannot seal this header");
 if (!prefix.empty()) std::memcpy(region_.address(), prefix.data(), prefix.size());
 if (::msync(region_.address(), bytes_, MS_SYNC) != 0) throw common_io::errno_error("cannot sync benchmark mapped artifact", staged_.path().string());
 // Reopening checks physical identity; a replaced path cannot acquire the
 // admission or publication custody of the mapping's original inode.
 auto file = common_io::FileHandle::open_readonly(staged_.path().string());
 struct stat identity{};
 if (::fstat(file.get(), &identity) != 0) throw common_io::errno_error("cannot inspect benchmark mapped artifact", staged_.path().string());
 if (identity.st_dev != device_ || identity.st_ino != inode_ || identity.st_size < 0 || static_cast<std::uint64_t>(identity.st_size) != bytes_)
  throw std::runtime_error("benchmark mapped artifact changed before sealing");
 file.sync_data();
 sealed_ = true;
}
void BenchmarkMappedArtifact::publish(const std::filesystem::path& destination, mmltk::common::concurrency::CancellationObservation cancellation) {
 if (!sealed_ || !can_publish(destination)) throw std::logic_error("benchmark mapped artifact is not publishable");
 staged_.publish(destination, cancellation);
 published_ = true;
}
}  // namespace mmltk::backend::data::benchmark_internal
