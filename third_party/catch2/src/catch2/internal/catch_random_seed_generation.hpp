

#ifndef CATCH_RANDOM_SEED_GENERATION_HPP_INCLUDED
#define CATCH_RANDOM_SEED_GENERATION_HPP_INCLUDED

#include <cstdint>

namespace Catch {

enum class GenerateFrom : std::uint8_t {
    Time,
    RandomDevice,
    Default
};

std::uint32_t generateRandomSeed(GenerateFrom from);

}  

#endif  // CATCH_RANDOM_SEED_GENERATION_HPP_INCLUDED
