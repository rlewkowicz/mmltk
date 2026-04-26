
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_RANDOM_NUMBER_GENERATOR_HPP_INCLUDED
#define CATCH_RANDOM_NUMBER_GENERATOR_HPP_INCLUDED

#include <cstdint>

namespace Catch {

class SimplePcg32 {
    using state_type = std::uint64_t;

   public:
    using result_type = std::uint32_t;
    static constexpr result_type(min)() {
        return 0;
    }
    static constexpr result_type(max)() {
        return static_cast<result_type>(-1);
    }

    SimplePcg32() : SimplePcg32(0xed743cc4U) {}

    explicit SimplePcg32(result_type seed_);

    void seed(result_type seed_);
    void discard(uint64_t skip);

    result_type operator()();

   private:
    friend bool operator==(SimplePcg32 const& lhs, SimplePcg32 const& rhs);
    friend bool operator!=(SimplePcg32 const& lhs, SimplePcg32 const& rhs);

    std::uint64_t m_state;
    static const std::uint64_t s_inc = (0x13ed0cc53f939476ULL << 1ULL) | 1ULL;
};

}  // namespace Catch

#endif  // CATCH_RANDOM_NUMBER_GENERATOR_HPP_INCLUDED
