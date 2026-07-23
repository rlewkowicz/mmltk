

#ifndef CATCH_ENVIRONMENT_HPP_INCLUDED
#define CATCH_ENVIRONMENT_HPP_INCLUDED

#include <catch2/benchmark/catch_clock.hpp>
#include <catch2/benchmark/catch_outlier_classification.hpp>

namespace Catch {
namespace Benchmark {
struct EnvironmentEstimate {
    FDuration mean;
    OutlierClassification outliers;
};
struct Environment {
    EnvironmentEstimate clock_resolution;
    EnvironmentEstimate clock_cost;
};
}  
}  

#endif  // CATCH_ENVIRONMENT_HPP_INCLUDED
