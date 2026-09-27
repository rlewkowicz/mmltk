#pragma once
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/io/file_memory.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkStagedArtifact final {
public:
 enum class Publication { Rename, RenameAndSync, DurableReplace };
 BenchmarkStagedArtifact() = default;
 BenchmarkStagedArtifact(const BenchmarkStagedArtifact&) = delete;
 BenchmarkStagedArtifact& operator=(const BenchmarkStagedArtifact&) = delete;
 BenchmarkStagedArtifact(BenchmarkStagedArtifact&&) noexcept;
 BenchmarkStagedArtifact& operator=(BenchmarkStagedArtifact&&) noexcept;
 ~BenchmarkStagedArtifact();
 [[nodiscard]] static BenchmarkStagedArtifact create(StorageReservationPool&, const std::filesystem::path& destination,
  std::uint64_t promised, std::string_view description, std::string_view suffix = ".tmp.XXXXXX", mode_t mode = 0644);
 [[nodiscard]] mmltk::common::io::FileHandle& file() noexcept { return file_; }
 [[nodiscard]] const mmltk::common::io::FileHandle& file() const noexcept { return file_; }
 [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
 // Storage-changing operations couple the physical mutation to its promise.
 void preallocate(std::size_t bytes);
 void resize(std::size_t bytes, std::string_view description);
 void reconcile();
 void close();
 void publish(const std::filesystem::path& destination, mmltk::common::concurrency::CancellationObservation, Publication = Publication::DurableReplace, bool overwrite = true);
private:
 void discard() noexcept;
 StorageReservationPool::Reservation allocation_;
 std::filesystem::path path_;
 std::string temporary_;
 bool cleanup_ = false;
 mmltk::common::io::FileHandle file_;
};
}  // namespace mmltk::backend::data::benchmark_internal
