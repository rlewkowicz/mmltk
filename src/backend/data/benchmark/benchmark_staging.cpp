#include "src/backend/data/benchmark/detail/benchmark_staging.h"
#include <sys/mman.h>
#include <stdexcept>
namespace mmltk::backend::data::benchmark_internal {
namespace common_io = mmltk::common::io;
BenchmarkMappedArtifact::BenchmarkMappedArtifact(detail::StorageReservationPool& storage, const std::filesystem::path& path, std::size_t bytes, std::string_view description) : bytes_(bytes) {
 if (!bytes) return;
 staged_ = detail::StagedArtifact::create(storage, path, bytes, description);
 staged_.resize(bytes, description);
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
 mapped_ = true;
 return region_.address();
}
const void* BenchmarkMappedArtifact::map_readonly() { return map(PROT_READ, MAP_PRIVATE); }
void* BenchmarkMappedArtifact::map_writable() { return map(PROT_READ | PROT_WRITE, MAP_SHARED); }
}  // namespace mmltk::backend::data::benchmark_internal
