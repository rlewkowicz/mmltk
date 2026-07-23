

#ifndef CATCH_GENERATORS_RANDOM_HPP_INCLUDED
#define CATCH_GENERATORS_RANDOM_HPP_INCLUDED

#include <catch2/generators/catch_generators.hpp>
#include <catch2/internal/catch_random_number_generator.hpp>
#include <catch2/internal/catch_uniform_integer_distribution.hpp>
#include <catch2/internal/catch_uniform_floating_point_distribution.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>

namespace Catch {
namespace Generators {
namespace Detail {
std::uint32_t getSeed();
}

template <typename Float>
class RandomFloatingGenerator final : public IGenerator<Float> {
    Catch::SimplePcg32 m_rng;
    Catch::uniform_floating_point_distribution<Float> m_dist;
    Float m_current_number;

   public:
    RandomFloatingGenerator(Float a, Float b, std::uint32_t seed) : m_rng(seed), m_dist(a, b) {
        static_cast<void>(next());
    }

    Float const& get() const override {
        return m_current_number;
    }
    bool next() override {
        m_current_number = m_dist(m_rng);
        return true;
    }
    bool isFinite() const override {
        return false;
    }
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
class RandomIntegerGenerator final : public IGenerator<Integer> {
    Catch::SimplePcg32 m_rng;
    Catch::uniform_integer_distribution<Integer> m_dist;
    Integer m_current_number;

   public:
    RandomIntegerGenerator(Integer a, Integer b, std::uint32_t seed) : m_rng(seed), m_dist(a, b) {
        static_cast<void>(next());
    }

    Integer const& get() const override {
        return m_current_number;
    }
    bool next() override {
        m_current_number = m_dist(m_rng);
        return true;
    }
    bool isFinite() const override {
        return false;
    }
};

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
