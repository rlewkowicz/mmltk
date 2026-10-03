#pragma once
#include "src/backend/data/detail/storage_reservation.h"
#include "src/common/io/file_memory.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <string_view>
#include <sys/types.h>
namespace mmltk::backend::data::detail {
class StagedArtifact final {
public:
 enum class Publication { Rename, RenameAndSync, DurableReplace };
 StagedArtifact() = default;
 StagedArtifact(const StagedArtifact&) = delete;
 StagedArtifact& operator=(const StagedArtifact&) = delete;
 StagedArtifact(StagedArtifact&&) noexcept;
 StagedArtifact& operator=(StagedArtifact&&) noexcept;
 ~StagedArtifact();
 [[nodiscard]] static StagedArtifact create(StorageReservationPool&, const std::filesystem::path& destination, std::uint64_t promised, std::string_view description,
  std::string_view suffix = ".tmp.XXXXXX", mode_t mode = 0644, int directory = -1);
 [[nodiscard]] mmltk::common::io::FileHandle& file() noexcept { return file_; }
 [[nodiscard]] const mmltk::common::io::FileHandle& file() const noexcept { return file_; }
 [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
 // Storage-changing operations couple the physical mutation to its promise.
 void preallocate(std::size_t bytes);
 void resize(std::size_t bytes, std::string_view description);
 void resize_sparse(std::size_t bytes, std::uint64_t* syscalls = nullptr, common::concurrency::CancellationObservation = {});
 void allocate_range(std::size_t offset, std::size_t bytes);
 void allocate_ranges(std::span<const common::io::FileHandle::Range>, common::concurrency::CancellationObservation = {}, std::uint64_t* syscalls = nullptr);
 [[nodiscard]] bool reclaim_range(std::size_t offset, std::size_t bytes);
 [[nodiscard]] bool reclaim_ranges(std::span<const common::io::FileHandle::Range>, common::concurrency::CancellationObservation = {}, std::uint64_t* syscalls = nullptr);
 void reconcile();
 void close();
 void publish(const std::filesystem::path& destination, mmltk::common::concurrency::CancellationObservation, Publication = Publication::DurableReplace, bool overwrite = true);

private:
 struct Mutation {
  std::mutex mutex;
  std::map<std::size_t, std::size_t> occupied;
 };
 void discard() noexcept;
 [[nodiscard]] std::uint64_t reconcile_locked();
 void forget_range(std::size_t begin, std::size_t end, std::map<std::size_t, std::size_t>& prepared);
 void occupy_range(std::size_t begin, std::size_t end, std::map<std::size_t, std::size_t>& prepared);
 std::unique_ptr<Mutation> mutation_;
 StorageReservationPool::Reservation allocation_;
 std::filesystem::path path_;
 std::string temporary_;
 bool cleanup_ = false;
 // Optional borrowed directory; its owner outlives this staged publication.
 int directory_ = -1;
 mmltk::common::io::FileHandle file_;
};
}  // namespace mmltk::backend::data::detail
