

#ifndef CATCH_UNIFORM_FLOATING_POINT_DISTRIBUTION_HPP_INCLUDED
#define CATCH_UNIFORM_FLOATING_POINT_DISTRIBUTION_HPP_INCLUDED

#include <catch2/internal/catch_random_floating_point_helpers.hpp>
#include <catch2/internal/catch_uniform_integer_distribution.hpp>

#include <cmath>
#include <type_traits>

namespace Catch {

namespace Detail {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
constexpr std::uint64_t calculate_max_steps_in_one_go(double gamma) {
    if (gamma == 1.99584030953472e+292) {
        return 9007199254740991;
    }
    return static_cast<std::uint64_t>(-1);
}
constexpr std::uint32_t calculate_max_steps_in_one_go(float gamma) {
    if (gamma == 2.028241e+31f) {
        return 16777215;
    }
    return static_cast<std::uint32_t>(-1);
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}  

template <typename FloatType>
class uniform_floating_point_distribution {
    static_assert(std::is_floating_point<FloatType>::value, "...");
    static_assert(!std::is_same<FloatType, long double>::value,
                  "We do not support long double due to inconsistent behaviour between platforms");

    using WidthType = Detail::DistanceType<FloatType>;

    FloatType m_a, m_b;
    FloatType m_ulp_magnitude;
    WidthType m_floats_in_range;
    uniform_integer_distribution<WidthType> m_int_dist;

    WidthType m_max_steps_in_one_go;
    bool m_a_has_leq_magnitude;

   public:
    using result_type = FloatType;

    uniform_floating_point_distribution(FloatType a, FloatType b)
        : m_a(a),
          m_b(b),
          m_ulp_magnitude(Detail::gamma(m_a, m_b)),
          m_floats_in_range(Detail::count_equidistant_floats(m_a, m_b, m_ulp_magnitude)),
          m_int_dist(0, m_floats_in_range),
          m_max_steps_in_one_go(Detail::calculate_max_steps_in_one_go(m_ulp_magnitude)),
          m_a_has_leq_magnitude(std::fabs(m_a) <= std::fabs(m_b)) {
        assert(a <= b);
    }

    template <typename Generator>
    result_type operator()(Generator& g) {
        WidthType steps = m_int_dist(g);
        if (m_a_has_leq_magnitude) {
            if (steps == m_floats_in_range) {
                return m_a;
            }
            auto b = m_b;
            while (steps > m_max_steps_in_one_go) {
                b -= m_max_steps_in_one_go * m_ulp_magnitude;
                steps -= m_max_steps_in_one_go;
            }
            return b - steps * m_ulp_magnitude;
        } else {
            if (steps == m_floats_in_range) {
                return m_b;
            }
            auto a = m_a;
            while (steps > m_max_steps_in_one_go) {
                a += m_max_steps_in_one_go * m_ulp_magnitude;
                steps -= m_max_steps_in_one_go;
            }
            return a + steps * m_ulp_magnitude;
        }
    }

    result_type a() const {
        return m_a;
    }
    result_type b() const {
        return m_b;
    }
};

}  

#endif  // CATCH_UNIFORM_FLOATING_POINT_DISTRIBUTION_HPP_INCLUDED
