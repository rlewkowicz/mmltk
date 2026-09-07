

#ifndef CATCH_GENERATORS_RANDOM_HPP_INCLUDED
#define CATCH_GENERATORS_RANDOM_HPP_INCLUDED

#include <catch2/generators/catch_generators.hpp>
#include <catch2/internal/catch_random_number_generator.hpp>
#include <catch2/internal/catch_uniform_floating_point_distribution.hpp>
#include <catch2/internal/catch_uniform_integer_distribution.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>

namespace Catch {
namespace Generators {
namespace Detail {
std::uint32_t getSeed();
}

// Shared implementation for random generators: draws values of type T from
// Distribution seeded through a SimplePcg32.
template <typename T, typename Distribution>
class RandomUniformGenerator : public IGenerator<T> {
    Catch::SimplePcg32 m_rng;
    Distribution m_dist;
    T m_current_number;

    // Non-virtual so the constructor can seed the first value without a virtual call.
    void draw_next() {
        m_current_number = m_dist(m_rng);
    }

   public:
    RandomUniformGenerator(T a, T b, std::uint32_t seed) : m_rng(seed), m_dist(a, b) {
        draw_next();
    }

    T const& get() const override {
        return m_current_number;
    }
    bool next() override {
        draw_next();
        return true;
    }
    bool isFinite() const override {
        return false;
    }
};

template <typename Float>
class RandomFloatingGenerator final
    : public RandomUniformGenerator<Float, Catch::uniform_floating_point_distribution<Float>> {
   public:
    using RandomUniformGenerator<Float, Catch::uniform_floating_point_distribution<Float>>::RandomUniformGenerator;
};

template <>
class RandomFloatingGenerator<long double> final : public IGenerator<long double> {
    struct PImpl;
    Catch::Detail::unique_ptr<PImpl> m_pimpl;
    long double m_current_number;

   public:
    RandomFloatingGenerator(long double a, long double b, std::uint32_t seed);

    long double const& get() const override {
        return m_current_number;
    }
    bool next() override;

    ~RandomFloatingGenerator() override;
    bool isFinite() const override;
};

template <typename Integer>
using RandomIntegerGenerator = RandomUniformGenerator<Integer, Catch::uniform_integer_distribution<Integer>>;

template <typename T>
std::enable_if_t<std::is_integral<T>::value, GeneratorWrapper<T>> random(T a, T b) {
    return GeneratorWrapper<T>(Catch::Detail::make_unique<RandomIntegerGenerator<T>>(a, b, Detail::getSeed()));
}

template <typename T>
std::enable_if_t<std::is_floating_point<T>::value, GeneratorWrapper<T>> random(T a, T b) {
    return GeneratorWrapper<T>(Catch::Detail::make_unique<RandomFloatingGenerator<T>>(a, b, Detail::getSeed()));
}

}
}

#endif  // CATCH_GENERATORS_RANDOM_HPP_INCLUDED
