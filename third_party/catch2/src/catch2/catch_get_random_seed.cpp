

#include <catch2/catch_get_random_seed.hpp>

#include <catch2/internal/catch_context.hpp>
#include <catch2/catch_config.hpp>

namespace Catch {
std::uint32_t getSeed() {
    return getCurrentContext().getConfig()->rngSeed();
}
}  
