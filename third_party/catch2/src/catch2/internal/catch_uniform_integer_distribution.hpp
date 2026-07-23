

#ifndef CATCH_UNIFORM_INTEGER_DISTRIBUTION_HPP_INCLUDED
#define CATCH_UNIFORM_INTEGER_DISTRIBUTION_HPP_INCLUDED

#include <catch2/internal/catch_random_integer_helpers.hpp>

namespace Catch {

template <typename IntegerType>
class uniform_integer_distribution {
    static_assert(std::is_integral<IntegerType>::value, "...");

    using UnsignedIntegerType = Detail::SizedUnsignedType_t<sizeof(IntegerType)>;

    UnsignedIntegerType m_a;

    UnsignedIntegerType m_ab_distance;

    UnsignedIntegerType m_rejection_threshold = 0;

    static constexpr UnsignedIntegerType computeDistance(IntegerType a, IntegerType b) {
        return transposeTo(b) - transposeTo(a) + 1;
    }

    // cppcheck-suppress passedByValue
    static constexpr UnsignedIntegerType computeRejectionThreshold(UnsignedIntegerType ab_distance) {
        if (ab_distance == 0) {
            return 0;
        }
        return (~ab_distance + 1) % ab_distance;
    }

    static constexpr UnsignedIntegerType transposeTo(IntegerType in) {
        return Detail::transposeToNaturalOrder<IntegerType>(static_cast<UnsignedIntegerType>(in));
    }
    static constexpr IntegerType transposeBack(UnsignedIntegerType in) {
        return static_cast<IntegerType>(Detail::transposeToNaturalOrder<IntegerType>(in));
    }

   public:
    using result_type = IntegerType;

    constexpr uniform_integer_distribution(IntegerType a, IntegerType b)
        : m_a(transposeTo(a)),
          m_ab_distance(computeDistance(a, b)),
          m_rejection_threshold(computeRejectionThreshold(m_ab_distance)) {
        assert(a <= b);
    }

    template <typename Generator>
    constexpr result_type operator()(Generator& g) {
        if (m_ab_distance == 0) {
            return transposeBack(Detail::fillBitsFrom<UnsignedIntegerType>(g));
        }

        auto random_number = Detail::fillBitsFrom<UnsignedIntegerType>(g);
        auto emul = Detail::extendedMult(random_number, m_ab_distance);
        while (emul.lower < m_rejection_threshold) {
            random_number = Detail::fillBitsFrom<UnsignedIntegerType>(g);
            emul = Detail::extendedMult(random_number, m_ab_distance);
        }

        return transposeBack(m_a + emul.upper);
    }

    constexpr result_type a() const {
        return transposeBack(m_a);
    }
    constexpr result_type b() const {
        return transposeBack(m_ab_distance + m_a - 1);
    }
};

}  

#endif  // CATCH_UNIFORM_INTEGER_DISTRIBUTION_HPP_INCLUDED
