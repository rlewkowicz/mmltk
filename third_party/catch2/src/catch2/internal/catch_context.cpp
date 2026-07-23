

#include <catch2/internal/catch_context.hpp>
#include <catch2/internal/catch_random_number_generator.hpp>

namespace Catch {

Context Context::currentContext;

Context& getCurrentMutableContext() {
    return Context::currentContext;
}

SimplePcg32& sharedRng() {
    static SimplePcg32 s_rng;
    return s_rng;
}

}  
