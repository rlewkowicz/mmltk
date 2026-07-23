

#include <catch2/benchmark/detail/catch_run_for_at_least.hpp>
#include <catch2/internal/catch_enforce.hpp>

#include <exception>

namespace Catch {
namespace Benchmark {
namespace Detail {
struct optimized_away_error : std::exception {
    const char* what() const noexcept override;
};

const char* optimized_away_error::what() const noexcept {
    return "could not measure benchmark, maybe it was optimized away";
}

void throw_optimized_away_error() {
    Catch::throw_exception(optimized_away_error{});
}

}  
}  
}  
