#pragma once
#include "src/backend/data/detail/staged_artifact.h"
#include "src/common/io/file_memory.h"
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <span>
#include <sys/types.h>
namespace mmltk::backend::data::benchmark_internal {
// Fixed temporary storage has one custody: mapping, backing file and physical
// reservation retire together. Mapping closes the construction descriptor.
class BenchmarkMappedArtifact final {
public:
 BenchmarkMappedArtifact(detail::StorageReservationPool&, const std::filesystem::path&, std::size_t bytes, std::string_view description);
 BenchmarkMappedArtifact(const BenchmarkMappedArtifact&) = delete;
 BenchmarkMappedArtifact& operator=(const BenchmarkMappedArtifact&) = delete;
 void write(const void* source, std::size_t bytes, std::size_t offset);
 [[nodiscard]] const void* map_readonly();
 [[nodiscard]] void* map_writable();
 // Only an unpublished backing on the destination filesystem can transfer
 // its inode. Callers serialize seal/publication against other destinations.
 [[nodiscard]] bool can_publish(const std::filesystem::path&) const;
 void seal_prefix(std::span<const std::byte>);
 void publish(const std::filesystem::path&, mmltk::common::concurrency::CancellationObservation);
 [[nodiscard]] const std::filesystem::path& path() const noexcept { return staged_.path(); }

private:
 [[nodiscard]] void* map(int protection, int flags);
 // Reverse destruction order unmaps before removing the file or its promise.
 detail::StagedArtifact staged_;
 mmltk::common::io::MappedByteRegion region_;
 std::size_t bytes_;
 bool mapped_ = false, writable_ = false, sealed_ = false, published_ = false;
 dev_t device_ = 0;
 ino_t inode_ = 0;
};
}  // namespace mmltk::backend::data::benchmark_internal
