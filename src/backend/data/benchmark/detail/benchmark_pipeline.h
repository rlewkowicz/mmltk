#pragma once
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCurl;
class BenchmarkResourceWait;
class BenchmarkSplitWriter;
struct PreparedBenchmarkSplit;
class StorageReservationPool;
// Only runnable CPU work enters this owner. Source controllers retain their I/O,
// locks and dependency waits outside its lanes. Stages are scheduling policy,
// never a replacement for recipe/source ownership or a product task graph.
class InsufficientBenchmarkResources : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
enum class BenchmarkStage : std::uint8_t { Metadata, Header, Pixels, Normalize, Recovery, Labels, Archive, CacheWrite, Count };
class BenchmarkCompilePipeline final {
 friend class BenchmarkSourcePublication;
 struct Admission;
 using Credits = BenchmarkAllowance::Credits;
 friend class BenchmarkAllowance;
 friend class BenchmarkResourceWait;
public:
 explicit BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus = {}, BenchmarkExecutionLimits = {},
  mmltk::common::concurrency::CancellationObservation = {});
 ~BenchmarkCompilePipeline();
 BenchmarkCompilePipeline(const BenchmarkCompilePipeline&) = delete;
 BenchmarkCompilePipeline& operator=(const BenchmarkCompilePipeline&) = delete;
 [[nodiscard]] std::size_t workers() const noexcept;
 [[nodiscard]] std::size_t current_lane() const;
 [[nodiscard]] BenchmarkCurl& curl();
 [[nodiscard]] StorageReservationPool& storage() noexcept;
 [[nodiscard]] std::span<const int> cpus() const noexcept;
 [[nodiscard]] std::uint64_t transient_target() const noexcept;
 [[nodiscard]] std::size_t descriptor_limit() const noexcept;
 // Idle source buffers yield to blocked consumers and resource borrowers.
 [[nodiscard]] bool resource_pressure() const;
 // Source controllers capture before checking readiness/resource fit, then
 // wait without holding source locks. Readiness publishers signal after their
 // own state changes; returned credits advance this same admission event.
 [[nodiscard]] std::uint64_t admission_generation() const;
 void wait_for_admission_change(std::uint64_t, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
 void notify_admission_change() noexcept;
 // A deferred I/O input observes this same admission event while keeping idle
 // scratch reclaimable. The token is retired on admission or abandonment.
 [[nodiscard]] std::unique_ptr<BenchmarkResourceWait> defer_resources();
 // Reject impossible controller demands before a nonblocking admission loop.
 void require_feasible(BenchmarkResources) const;
 [[nodiscard]] std::size_t descriptor_ceiling(BenchmarkResources) const;
 [[nodiscard]] std::optional<BenchmarkAllowance> try_reserve(BenchmarkResources, const BenchmarkAllowance& parent = {});
 // A dependent draw uses at most its parent's remaining commitment. Additional
 // demand needs uncommitted capacity; copied parents cannot lend twice.
 // Called by I/O/source owners only; never blocks a CPU lane on resources.
 [[nodiscard]] BenchmarkAllowance reserve(BenchmarkResources, const BenchmarkAllowance& parent = {});
 // Admit a complete HTTP envelope, reducing optional connections under pressure.
 [[nodiscard]] std::pair<std::size_t, BenchmarkAllowance> reserve_transfers(std::size_t requested, BenchmarkTransferEnvelope, const BenchmarkAllowance& parent = {});
 // The borrowed callback and its inputs remain live through completion/failure.
 // Input ownership supplies an allowance when a producer already reserved the
 // complete input/work/output footprint; no second charge is made.
 void run(BenchmarkStage, BenchmarkResources, const std::function<void(std::size_t)>&, BenchmarkAllowance = {});
 // Bounded records recycle on individual completion. Retirement finishes before
 // returning, including failure; it cannot retire suspended cooperative scratch.
 void for_each(BenchmarkStage, std::size_t count, BenchmarkResources, const std::function<void(std::size_t)>&, const std::function<void(std::size_t)>& retire_scratch = {});
 void write_remaining(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&, std::span<const std::size_t>);
 // A synchronous library parser yields between bounded progress points.
 // Runs only already-ready work on its current lane, without waiting.
 void cooperate();
 void membership_ready();
 void register_split(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&);
 // Capture once while the producer holds its physical mutation lease. A repair
 // captures only its replacement image; all other events keep the source epoch.
 [[nodiscard]] BenchmarkSourcePublication source_publication(const std::filesystem::path&, std::shared_ptr<const ArtifactLease>,
  std::optional<std::uint64_t> repaired_image = {}, bool defer_pixels = false);
 [[nodiscard]] BenchmarkSourceGeneration source_generation(const std::filesystem::path&);
 [[nodiscard]] BenchmarkSourceGeneration image_generation(const std::filesystem::path&, std::uint64_t);
 void retire_image(const std::filesystem::path&, std::uint64_t);
 void geometry_ready(const BenchmarkImageGeometry&);
 [[nodiscard]] std::shared_ptr<const BenchmarkEncodedImage> image_input(const std::filesystem::path&, std::uint64_t) const;
 [[nodiscard]] std::optional<BenchmarkImageGeometry> geometry(const std::filesystem::path&, std::uint64_t) const;
 // Withdraw admission first, join only this source, then clear affected facts.
 // Unrelated source tasks and completed products continue to be usable.
 void retire_source(const std::filesystem::path&);
 void drain();
 // Explicit borrower boundary for construction before recipe/writer owners.
 // Discards queued work and joins active calls before registered owners die.
 void retire_attempt() noexcept;
 class Attempt final {
 public:
  explicit Attempt(BenchmarkCompilePipeline& owner) : owner_(owner) {}
  ~Attempt() { owner_.retire_attempt(); }
  Attempt(const Attempt&) = delete;
  Attempt& operator=(const Attempt&) = delete;
 private:
  BenchmarkCompilePipeline& owner_;
 };
private:
 void retire_workspace(const void*) noexcept;
 struct Impl;
 std::shared_ptr<Impl> impl_;
};
class BenchmarkResourceWait final {
public:
 ~BenchmarkResourceWait();
 BenchmarkResourceWait(const BenchmarkResourceWait&) = delete;
 BenchmarkResourceWait& operator=(const BenchmarkResourceWait&) = delete;
private:
 friend class BenchmarkCompilePipeline;
 explicit BenchmarkResourceWait(std::shared_ptr<BenchmarkCompilePipeline::Admission>);
 std::shared_ptr<BenchmarkCompilePipeline::Admission> owner_;
};
}  // namespace mmltk::backend::data::benchmark_internal
