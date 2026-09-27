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
private:
 friend class BenchmarkCompilePipeline;
 explicit BenchmarkAllowance(std::shared_ptr<Credits> value) : credits_(std::move(value)) {}
 std::shared_ptr<Credits> credits_;
};
}  // namespace mmltk::backend::data::benchmark_internal
