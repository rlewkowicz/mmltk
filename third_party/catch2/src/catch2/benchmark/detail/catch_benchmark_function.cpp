

#include <catch2/benchmark/detail/catch_benchmark_function.hpp>

namespace Catch {
namespace Benchmark {
namespace Detail {
struct do_nothing {
    void operator()() const {}
};

BenchmarkFunction::callable::~callable() = default;
BenchmarkFunction::BenchmarkFunction() : f(new model<do_nothing>{{}}) {}
}  
}  
}  
