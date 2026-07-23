

#ifndef CATCH_ANALYSE_HPP_INCLUDED
#define CATCH_ANALYSE_HPP_INCLUDED

#include <catch2/benchmark/catch_clock.hpp>
#include <catch2/benchmark/catch_sample_analysis.hpp>

namespace Catch {
class IConfig;

namespace Benchmark {
namespace Detail {
SampleAnalysis analyse(const IConfig& cfg, FDuration* first, FDuration* last);
}
}  
}  

#endif  // CATCH_ANALYSE_HPP_INCLUDED
