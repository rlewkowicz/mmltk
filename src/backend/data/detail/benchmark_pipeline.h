#pragma once
#include "benchmark_images.h"
#include "benchmark_writer.h"
#include "src/common/concurrency/worker_pool.h"
#include <cstddef>
#include <condition_variable>
#include <exception>
#include <memory>
#include <span>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
// Compile-scoped CPU partition and bounded ready-pixel executor. Acquisition
// owns source generations; drain is its mandatory reader-retirement boundary.
class BenchmarkCompilePipeline final {
public:
 explicit BenchmarkCompilePipeline(std::size_t workers, std::span<const int> cpus = {});
 ~BenchmarkCompilePipeline();
 BenchmarkCompilePipeline(const BenchmarkCompilePipeline&) = delete;
 BenchmarkCompilePipeline& operator=(const BenchmarkCompilePipeline&) = delete;
 [[nodiscard]] std::size_t acquisition_workers() const noexcept;
 [[nodiscard]] std::size_t pixel_workers() const noexcept;
 [[nodiscard]] std::span<const int> acquisition_cpus() const noexcept;
 void register_split(BenchmarkSplitWriter&, const PreparedBenchmarkSplit&);
 void image_ready(const CachedImageReady&);
 void drain();

private:
 struct Slot {
  BenchmarkSplitWriter* writer;
  std::size_t index;
  bool submitted = false;
  Slot* next = nullptr;
  std::shared_ptr<const ArtifactLease> custody{};
 };
 void consume(std::size_t lane) noexcept;
 void execute(Slot&, std::size_t lane, std::shared_ptr<const ArtifactLease> custody) noexcept;
 void stop() noexcept;
 std::size_t workers_;
 std::size_t pixels_;
 std::vector<int> acquisition_cpus_;
 std::unordered_map<std::string, std::unordered_map<std::uint64_t, Slot>> slots_;
 std::mutex mutex_;
 std::condition_variable changed_;
 std::condition_variable work_ready_;
 Slot* ready_head_ = nullptr;
 Slot* ready_tail_ = nullptr;
 bool stopping_ = false;
 bool admitted_ = false;
 std::size_t pending_ = 0;
 std::exception_ptr failure_;
 std::unique_ptr<mmltk::common::concurrency::WorkerPool> pool_;
};
}  // namespace mmltk::backend::data::benchmark_internal
