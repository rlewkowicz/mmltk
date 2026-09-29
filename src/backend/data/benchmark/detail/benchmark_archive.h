#pragma once
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <array>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
// Rejected archive bytes are distinct from allocation, local I/O and storage failures.
class BenchmarkArchiveError : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
// One opened inode and decoder generation. Positions and gzip dictionary/bit
// state belong to this owner; no index export or validation walk finishes EOF.
class BenchmarkArchive final {
public:
 // Two-byte format inspection sizes source admission; it never enumerates or
 // decompresses members. The returned bound includes this reader's buffers.
 struct InputRequirement {
  std::uint64_t workspace_bytes = 0;
  std::size_t descriptors = 1;
 };
 [[nodiscard]] static InputRequirement input_requirement(const std::filesystem::path&, std::uint64_t consumer_workspace, std::size_t gzip_workers = 1);
 explicit BenchmarkArchive(const std::filesystem::path&, BenchmarkCompilePipeline* = nullptr, std::uint64_t workspace = 0, const BenchmarkAllowance& parent = {}, std::size_t gzip_workers = 1,
  bool verify_gzip_crc = true, BenchmarkAllowance workspace_allowance = {}, std::size_t retained_gzip_windows = 1024, std::size_t consumer_descriptors = 0, std::size_t gzip_index_entries = 32768);
 ~BenchmarkArchive();
 BenchmarkArchive(const BenchmarkArchive&) = delete;
 BenchmarkArchive& operator=(const BenchmarkArchive&) = delete;
 bool next(mmltk::common::concurrency::CancellationObservation = {});
 [[nodiscard]] const std::string& member() const;
 [[nodiscard]] bool regular() const;
 [[nodiscard]] std::uint64_t size() const;
 // Stable entry ordinal, independent of format-specific read/skip byte offsets.
 [[nodiscard]] std::uint64_t position() const;
 // Compile-local location authority. It carries the opened inode generation,
 // and only this reader can construct or validate it; it is never persisted.
 class MemberPosition {
  friend class BenchmarkArchive;
  std::array<std::uint64_t, 7> generation_{};
  std::string member_;
  std::uint64_t header_ = 0, bytes_ = 0, ordinal_ = 0;
  std::optional<std::uint64_t> extent_;
  bool raw_ = false;
 };
 [[nodiscard]] MemberPosition member_position() const;
 bool seek(const MemberPosition&, mmltk::common::concurrency::CancellationObservation = {});
 void require_regular(std::uint64_t limit) const;
 void consume(std::uint64_t limit, const std::function<void(std::span<const std::uint8_t>, std::uint64_t)>&, mmltk::common::concurrency::CancellationObservation = {});
 void read_into(std::span<std::uint8_t>, mmltk::common::concurrency::CancellationObservation = {});
 [[nodiscard]] std::span<const std::uint8_t> read(std::uint64_t limit, mmltk::common::concurrency::CancellationObservation = {});
 // Revisit an encountered member; raw safe extents use pread, gzip reuses its
 // live dictionary index while retained. Evicted state and context-dependent
 // formats explicitly reread the needed prefix.
 bool seek(std::string_view member, mmltk::common::concurrency::CancellationObservation = {});
 // Visit already encountered required members in physical order. The callback
 // receives its original input index and borrows this reader synchronously.
 // One format context spans the batch, including ZIP/extended/sparse tar and
 // paused or evicted gzip state. A missing/conflicting request is an error.
 void visit_known(std::span<const std::string> members, const std::function<void(std::size_t)>&, mmltk::common::concurrency::CancellationObservation = {});
 // Release decoder/buffer capacity between discovery and later consumption.
 // The opened inode and encountered positions stay bound to this generation.
 void pause();
 void resume(std::uint64_t workspace, std::size_t consumer_descriptors = 0);
 struct GzipSeekState {
  std::size_t dictionaries;
  bool retained;
  bool streaming;
  bool rolling;
  bool control_capacity_reached;
  std::size_t index_entries;
  std::size_t index_bytes;
 };
 // Effect-only snapshot: no index export, read, allocation, or cache decision.
 [[nodiscard]] GzipSeekState gzip_seek_state() const;
 // Full active decoder/prefetch envelope plus retained member capacity.
 // Calling read does not quiesce native workers, so none of their bound is lent.
 [[nodiscard]] std::uint64_t retained_workspace_bytes() const;
 [[nodiscard]] BenchmarkAllowance allowance() const;
 void cpu(const std::function<void()>&) const;

private:
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
[[nodiscard]] std::string canonical_benchmark_archive_member(std::string_view);
// Discovery only: exact namespace, spelling and payload admission belong to the consumer.
[[nodiscard]] std::optional<std::uint64_t> benchmark_archive_image_candidate(std::string_view);
}  // namespace mmltk::backend::data::benchmark_internal
