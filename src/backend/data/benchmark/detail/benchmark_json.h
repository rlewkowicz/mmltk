#pragma once
#include "src/backend/data/detail/compile_execution.h"
#include "src/common/io/file_memory.h"
#include "src/common/types/cancellation_observation.h"
#include <simdjson.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
using detail::CompileExecution;
struct ByteRange {
 std::size_t begin = 0, end = 0;
};
// Keep at least one row, then bound each parser scheduling window by bytes.
[[nodiscard]] inline std::size_t annotation_json_chunk_end(std::span<const ByteRange> rows, std::size_t begin) {
 auto end = begin + 1;
 while (end < rows.size() && rows[end].end - rows[begin].begin <= (256U << 10)) ++end;
 return end;
}
class PaddedMappedFile final {
public:
 explicit PaddedMappedFile(const std::filesystem::path&);
 ~PaddedMappedFile();
 PaddedMappedFile(const PaddedMappedFile&) = delete;
 PaddedMappedFile& operator=(const PaddedMappedFile&) = delete;
 const char* data() const noexcept { return data_; }
 std::size_t size() const noexcept { return size_; }
 std::size_t capacity_from(std::size_t) const;

private:
 mmltk::common::io::FileHandle file_;
 const char* data_ = nullptr;
 std::size_t size_ = 0, capacity_ = 0;
};
[[nodiscard]] bool consume_json_string_token(char, bool&, bool&) noexcept;
// One structural discovery walk. Selected envelope keys are decoded; callers
// choose their format's duplicate policy. Row byte positions belong to input.
// A callback borrows one bounded range batch and the selected field's index.
// Array completion may carry an empty batch. It always runs outside scan CPU
// custody; consumers retain ranges only for unresolved semantic dependencies.
void discover_json_arrays(const PaddedMappedFile&, std::span<const std::string_view>, bool reject_duplicates, const std::function<void(std::size_t, std::span<const ByteRange>, bool complete)>&,
 mmltk::common::concurrency::CancellationObservation = {}, CompileExecution* = nullptr, const std::function<void(const std::function<void()>&)>& metadata_work = {});
[[noreturn]] void reject_json_document(const simdjson::simdjson_error&);
}  // namespace mmltk::backend::data::benchmark_internal
