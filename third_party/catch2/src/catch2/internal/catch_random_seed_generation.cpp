

#include <catch2/internal/catch_random_seed_generation.hpp>

#include <catch2/internal/catch_enforce.hpp>
#include <catch2/internal/catch_random_integer_helpers.hpp>

#include <ctime>
#include <random>

namespace Catch {

std::uint32_t generateRandomSeed(GenerateFrom from) {
    switch (from) {
        case GenerateFrom::Time:
            return static_cast<std::uint32_t>(std::time(nullptr));

        case GenerateFrom::Default:
        case GenerateFrom::RandomDevice: {
            std::random_device rd;
            return Detail::fillBitsFrom<std::uint32_t>(rd);
        }

        default:
            CATCH_ERROR("Unknown generation method");
    }
}

}  
