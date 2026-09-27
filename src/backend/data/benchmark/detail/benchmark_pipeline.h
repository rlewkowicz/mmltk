#pragma once
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_storage.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
// Only runnable CPU work enters this owner. Source controllers retain their I/O,
// locks and dependency waits outside its lanes. Stages are scheduling policy,
// never a replacement for recipe/source ownership or a product task graph.
class InsufficientBenchmarkResources : public std::runtime_error {
public:
 using std::runtime_error::runtime_error;
};
enum class BenchmarkStage : std::uint8_t { Metadata, Header, Pixels, Normalize, Recovery, Labels, Archive, CacheWrite, Count };
struct BenchmarkResources {
 std::uint64_t bytes = 0;
 std::size_t descriptors = 0;
 // Producer allowances include their consumers' scratch/output, atomically.
 bool producer = false;
 // Fixed nonpreemptible library grant, including its calling CPU.
 std::size_t cpu_workers = 0;
 // Small retained file/lease control records are charged separately from the
 // workspace their consumer needs to finish. Never use this for data buffers.
 bool retained_handles = false;
 // Live descriptor commitment for explicitly bound dependent reservations.
 std::size_t continuation_descriptors = 0;
 [[nodiscard]] static BenchmarkResources handles(std::size_t count, bool producer = false, std::size_t continuation = 0);
};
struct BenchmarkExecutionLimits {
 std::uint64_t transient_bytes = 0;
 std::size_t descriptors = 0;
};
class BenchmarkCompilePipeline final {
 struct Admission;
 struct Credits;
public:
 // Shared custody charges a backing allocation once, including across handoffs.
 // Last release wakes resource admission; copying custody does not charge again.
 // Custody may outlive this execution owner without retaining workers/borrowers.
 class Allowance {
 public:
  Allowance() = default;
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(credits_); }
  [[nodiscard]] std::uint64_t bytes() const noexcept;
  [[nodiscard]] std::shared_ptr<ArtifactLease> retain(ArtifactLease) const;
 private:
  friend class BenchmarkCompilePipeline;
  explicit Allowance(std::shared_ptr<Credits> value) : credits_(std::move(value)) {}
  std::shared_ptr<Credits> credits_;
 };
 explicit BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus = {}, BenchmarkExecutionLimits = {},
  mmltk::common::concurrency::CancellationObservation = {});
 ~BenchmarkCompilePipeline();
 BenchmarkCompilePipeline(const BenchmarkCompilePipeline&) = delete;
 BenchmarkCompilePipeline& operator=(const BenchmarkCompilePipeline&) = delete;
 [[nodiscard]] std::size_t workers() const noexcept;
 [[nodiscard]] std::size_t current_lane() const;
 [[nodiscard]] StorageReservationPool& storage() noexcept;
 [[nodiscard]] std::span<const int> cpus() const noexcept;
 [[nodiscard]] std::uint64_t transient_target() const noexcept;
 [[nodiscard]] std::size_t descriptor_limit() const noexcept;
 // Source controllers capture before checking readiness/resource fit, then
 // wait without holding source locks. Readiness publishers signal after their
 // own state changes; returned credits advance this same admission event.
 [[nodiscard]] std::uint64_t admission_generation() const;
 void wait_for_admission_change(std::uint64_t);
 void notify_admission_change() noexcept;
 [[nodiscard]] std::optional<Allowance> try_reserve(BenchmarkResources, const Allowance& parent = {});
 // A dependent draw uses at most its parent's remaining commitment. Additional
 // demand needs uncommitted capacity; copied parents cannot lend twice.
 // Called by I/O/source owners only; never blocks a CPU lane on resources.
 [[nodiscard]] Allowance reserve(BenchmarkResources, const Allowance& parent = {});
 // Admit a complete HTTP envelope, reducing optional connections under pressure.
 [[nodiscard]] std::pair<std::size_t, Allowance> reserve_transfers(std::size_t requested, std::size_t fixed_descriptors, std::uint64_t bytes_per_connection, std::uint64_t fixed_bytes = 0, const Allowance& parent = {});
 // The borrowed callback and its inputs remain live through completion/failure.
 // Input ownership supplies an allowance when a producer already reserved the
 // complete input/work/output footprint; no second charge is made.
 void run(BenchmarkStage, BenchmarkResources, const std::function<void(std::size_t)>&, Allowance = {});
 // Bounded records recycle on individual completion. Retirement finishes before
 // returning, including failure; it cannot retire suspended cooperative scratch.
 void for_each(BenchmarkStage, std::size_t count, BenchmarkResources, const std::function<void(std::size_t)>&, const std::function<void(std::size_t)>& retire_scratch = {});
 void write_remaining(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&, std::span<const std::size_t>);
 // A synchronous library parser yields between bounded progress points.
 // Runs only already-ready work on its current lane, without waiting.
 void cooperate();
 void membership_ready();
 void register_split(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&);
 void image_ready(const CachedImageReady&);
 [[nodiscard]] BenchmarkSourceGeneration source_generation(const std::filesystem::path&);
 [[nodiscard]] BenchmarkSourceGeneration image_generation(const std::filesystem::path&, std::uint64_t);
 void retire_image(const std::filesystem::path&, std::uint64_t);
 void geometry_ready(const BenchmarkImageGeometry&);
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
}  // namespace mmltk::backend::data::benchmark_internal
