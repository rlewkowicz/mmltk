

#ifndef CATCH_EXECUTION_PLAN_HPP_INCLUDED
#define CATCH_EXECUTION_PLAN_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/benchmark/catch_clock.hpp>
#include <catch2/benchmark/catch_environment.hpp>
#include <catch2/benchmark/detail/catch_benchmark_function.hpp>
#include <catch2/benchmark/detail/catch_repeat.hpp>
#include <catch2/benchmark/detail/catch_run_for_at_least.hpp>

#include <vector>

namespace Catch {
namespace Benchmark {
struct ExecutionPlan {
    int iterations_per_sample;
    FDuration estimated_duration;
    Detail::BenchmarkFunction benchmark;
    FDuration warmup_time;
    int warmup_iterations;

    template <typename Clock>
    std::vector<FDuration> run(const IConfig& cfg, Environment env) const {
        Detail::run_for_at_least<Clock>(std::chrono::duration_cast<IDuration>(warmup_time), warmup_iterations,
                                        Detail::repeat([]() { return Clock::now(); }));

        std::vector<FDuration> times;
        const auto num_samples = cfg.benchmarkSamples();
        times.reserve(num_samples);
        for (size_t i = 0; i < num_samples; ++i) {
            Detail::ChronometerModel<Clock> model;
            this->benchmark(Chronometer(model, iterations_per_sample));
            auto sample_time = model.elapsed() - env.clock_cost.mean;
            if (sample_time < FDuration::zero()) {
                sample_time = FDuration::zero();
            }
            times.push_back(sample_time / iterations_per_sample);
        }
        return times;
    }
};
}  
}  

#endif  // CATCH_EXECUTION_PLAN_HPP_INCLUDED
