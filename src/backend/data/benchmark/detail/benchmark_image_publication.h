#pragma once
#include "src/backend/data/benchmark/detail/benchmark_archive.h"
#include "src/backend/data/benchmark/detail/benchmark_image_input.h"
#include "src/backend/data/benchmark/detail/benchmark_image_facts.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/detail/storage_reservation.h"
#include "src/backend/data/detail/compile_execution.h"
#include "src/backend/data/detail/compile_resources.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
// One controller copies forward archive members into bounded admitted storage.
// Persistent file services own accepted writes; ready runs on their completion
// thread, while settled runs only on the controller through the completion
// window. Source identity, generation, repair and eligibility stay with callers.
class BenchmarkImagePublication final {
 struct Buffers {
  std::mutex mutex;
  std::size_t free = 0;
  detail::CompileExecution* execution = nullptr;
 };

public:
 struct Payload {
  Payload() = default;
  ~Payload();
  Payload(const Payload&) = delete;
  Payload& operator=(const Payload&) = delete;
  detail::CompileAllowance allowance;
  std::span<std::uint8_t> encoded;

 private:
  friend class BenchmarkImagePublication;
  std::shared_ptr<Buffers> pool;
  detail::CompileAllowance output;
  bool restored_decoder = false;
  std::vector<std::uint8_t> standalone;
 };
 using Input = std::shared_ptr<Payload>;
 using Settled = std::function<void(const CachedImageReady&, std::exception_ptr)>;
 BenchmarkImagePublication(std::filesystem::path, mmltk::common::concurrency::CancellationObservation, detail::StorageReservationPool*, detail::CompileAllowance, BenchmarkCompilePipeline*,
  int directory, bool retain_encoded, BenchmarkArchive&, const BenchmarkTraceSink&);
 ~BenchmarkImagePublication();
 BenchmarkImagePublication(const BenchmarkImagePublication&) = delete;
 BenchmarkImagePublication& operator=(const BenchmarkImagePublication&) = delete;
 [[nodiscard]] Input acquire(std::size_t);
 void read(const Input&, const BenchmarkArchive::MemberPosition* = nullptr);
 // Both callbacks, and their captured source tickets, exist before acceptance.
 // No callback borrows the archive cursor. Rejection retains caller settlement.
 void submit(std::uint64_t, Input, BenchmarkImageHeader, CachedImageReadySink, Settled);
 void reap();
 void finish();

private:
 BenchmarkCompilePipeline* execution_;
 int directory_;
 bool retain_encoded_;
 detail::StorageReservationPool storage_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 detail::CompileAllowance parent_;
 BenchmarkArchive& reader_;
 BenchmarkTraceSink trace_;
 std::shared_ptr<Buffers> buffers_ = std::make_shared<Buffers>();
 std::mutex resource_mutex_;
 std::condition_variable resource_changed_;
 std::uint64_t resource_generation_ = 0;
 std::unique_ptr<detail::CompileExecution::Readiness> readiness_;
 BenchmarkCompletionWindow writes_;
};
}  // namespace mmltk::backend::data::benchmark_internal
