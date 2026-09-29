#pragma once
#include "src/common/io/file_memory.h"
#include "src/common/concurrency/cancellation_observation.h"
#include <simdjson.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
class BenchmarkCompilePipeline;
struct ByteRange {
 std::size_t begin = 0, end = 0;
};
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
 mmltk::common::concurrency::CancellationObservation = {}, BenchmarkCompilePipeline* = nullptr);
[[noreturn]] void reject_json_document(const simdjson::simdjson_error&);
}  // namespace mmltk::backend::data::benchmark_internal
