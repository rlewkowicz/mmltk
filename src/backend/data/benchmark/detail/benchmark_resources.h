#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
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
// Quantified fixed and per-transfer demand; transport policy belongs to Curl.
struct BenchmarkTransferEnvelope {
 BenchmarkResources fixed;
 BenchmarkResources per_transfer;
 [[nodiscard]] BenchmarkResources demand(std::size_t transfers) const;
};
// Shared custody charges backing once and returns it after its last reader.
class BenchmarkAllowance {
 struct Credits;

public:
 BenchmarkAllowance() = default;
 [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(credits_); }
 [[nodiscard]] std::uint64_t bytes() const noexcept;
 [[nodiscard]] std::size_t descriptors() const noexcept;
 [[nodiscard]] bool aliases(const BenchmarkAllowance& other) const noexcept { return credits_ && credits_ == other.credits_; }
 // At a settled owner boundary, resize only this credit's unused promise.
 // The caller preserves its live backing envelope; split children stay charged
 // independently. Growth never waits and failure leaves custody unchanged.
 // retain_capacity preserves high water only while no resource demand is blocked.
 [[nodiscard]] bool try_resize_workspace(std::uint64_t, bool retain_capacity = false) const;
 // The backing owner calls this only after joining its workers and releasing
 // its own storage. Aliases keep descriptor commitments; split storage children
 // keep their separately charged bytes. External CPU capacity retires here too.
 // Accounting waits for any remaining CPU frames and stable offer scope to
 // settle; the request never releases physical storage on the owner's behalf.
 void retire_workspace() const noexcept;
 // A settled producer may return its unused continuation while retaining its
 // actual open files. Every continuation child must already have returned.
 void retire_continuation() const;
 // After physically closing this owner's files and completing its descriptor
 // production, return its draw and unused promises. Published consumers must
 // already own their continuation. Live child returns pass through this retired
 // owner; aliases and split storage cannot return the same promise twice.
 void retire_descriptors() const noexcept;
 // Partition an already admitted storage envelope, without another ledger
 // charge. Use before publishing the allowance to readers and outside any
 // unused-workspace offer. Positive storage keeps its own workspace loans;
 // descriptor-only aliases retain none after the backing owner retires bytes.
 [[nodiscard]] BenchmarkAllowance split_storage(std::uint64_t);

private:
 friend class BenchmarkCompilePipeline;
 explicit BenchmarkAllowance(std::shared_ptr<Credits> value) : credits_(std::move(value)) {}
 std::shared_ptr<Credits> credits_;
};
}  // namespace mmltk::backend::data::benchmark_internal
