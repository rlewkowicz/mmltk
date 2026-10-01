#pragma once
#include "src/backend/data/detail/compile_resources.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include <arrow/memory_pool.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
// Arrow payload is logically capped independently of each pinned physical
// window. Quiesced allocations keep their original addresses and full custody.
class CoconutParquetPool final : public arrow::MemoryPool {
 struct Window;
 mutable std::mutex mutex_;
 std::vector<std::shared_ptr<Window>> windows_;
 std::shared_ptr<Window> active_;
 std::int64_t live_ = 0, peak_ = 0, total_ = 0, allocations_ = 0;
 std::size_t fragmented_extent_ = 0;
 arrow::Status allocate(std::int64_t, std::int64_t, std::uint8_t**, std::int64_t replaced = 0);
 void free(std::uint8_t*, std::int64_t);

public:
 explicit CoconutParquetPool(detail::CompileAllowance, std::size_t standalone_bytes = kParquetPoolBytes);
 void resume(detail::CompileAllowance, std::size_t standalone_bytes = kParquetPoolBytes);
 void quiesce();
 void close() { quiesce(); }
 // One library entry owns allocation admission; concurrent immutable borrowers
 // may only Free. A fit failure is distinct even if Parquet wraps Arrow status.
 void begin_call();
 [[nodiscard]] std::size_t fragmented_extent() const;
 [[nodiscard]] std::uint64_t retained_capacity() const;
 arrow::Status Allocate(std::int64_t, std::int64_t, std::uint8_t**) override;
 arrow::Status Reallocate(std::int64_t, std::int64_t, std::int64_t, std::uint8_t**) override;
 void Free(std::uint8_t*, std::int64_t, std::int64_t) override;
 std::int64_t bytes_allocated() const override;
 std::int64_t max_memory() const override;
 std::int64_t total_bytes_allocated() const override;
 std::int64_t num_allocations() const override;
 std::string backend_name() const override;
};
}  // namespace mmltk::backend::data::benchmark_internal
