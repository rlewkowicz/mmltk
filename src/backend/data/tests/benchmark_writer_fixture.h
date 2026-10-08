#pragma once
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/tests/test_fixture.h"
#include <filesystem>
#include <utility>
namespace mmltk::backend::data::testsupport {
// Fixed slots borrow the split; the attempt retires them before their writer.
struct BoundBenchmarkWriter {
 benchmark_internal::BenchmarkWriteRequest request;
 benchmark_internal::BenchmarkSplitWriter writer;
 benchmark_internal::BenchmarkCompilePipeline::Attempt attempt;
 BoundBenchmarkWriter(benchmark_internal::BenchmarkCompilePipeline& execution, benchmark_internal::BenchmarkWriteRequest input) : request(std::move(input)), writer(request), attempt(execution) {
  bind_benchmark_slots(execution, writer, request.split);
 }
 BoundBenchmarkWriter(benchmark_internal::BenchmarkCompilePipeline& execution, benchmark_internal::BenchmarkWriteRequest input, const std::filesystem::path& images, AnnotationSource source)
     : request(std::move(input)), writer(request, true), attempt(execution) {
  execution.bind_source(images, writer, source);
 }
};
}  // namespace mmltk::backend::data::testsupport
