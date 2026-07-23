

#ifndef CATCH_ESTIMATE_HPP_INCLUDED
#define CATCH_ESTIMATE_HPP_INCLUDED

namespace Catch {
namespace Benchmark {
template <typename Type>
struct Estimate {
    Type point;
    Type lower_bound;
    Type upper_bound;
    double confidence_interval;
};
}  
}  

#endif  // CATCH_ESTIMATE_HPP_INCLUDED
