
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_MATCHERS_FLOATING_POINT_HPP_INCLUDED
#define CATCH_MATCHERS_FLOATING_POINT_HPP_INCLUDED

#include <catch2/matchers/catch_matchers.hpp>

namespace Catch {
namespace Matchers {

namespace Detail {
enum class FloatingPointKind : uint8_t;
}

class WithinAbsMatcher final : public MatcherBase<double> {
   public:
    WithinAbsMatcher(double target, double margin);
    bool match(double const& matchee) const override;
    std::string describe() const override;

   private:
    double m_target;
    double m_margin;
};

WithinAbsMatcher WithinAbs(double target, double margin);

class WithinUlpsMatcher final : public MatcherBase<double> {
   public:
    WithinUlpsMatcher(double target, uint64_t ulps, Detail::FloatingPointKind baseType);
    bool match(double const& matchee) const override;
    std::string describe() const override;

   private:
    double m_target;
    uint64_t m_ulps;
    Detail::FloatingPointKind m_type;
};

WithinUlpsMatcher WithinULP(double target, uint64_t maxUlpDiff);
WithinUlpsMatcher WithinULP(float target, uint64_t maxUlpDiff);

class WithinRelMatcher final : public MatcherBase<double> {
   public:
    WithinRelMatcher(double target, double epsilon);
    bool match(double const& matchee) const override;
    std::string describe() const override;

   private:
    double m_target;
    double m_epsilon;
};

WithinRelMatcher WithinRel(double target, double eps);
WithinRelMatcher WithinRel(double target);
WithinRelMatcher WithinRel(float target, float eps);
WithinRelMatcher WithinRel(float target);

class IsNaNMatcher final : public MatcherBase<double> {
   public:
    IsNaNMatcher() = default;
    bool match(double const& matchee) const override;
    std::string describe() const override;
};

IsNaNMatcher IsNaN();

}  // namespace Matchers
}  // namespace Catch

#endif  // CATCH_MATCHERS_FLOATING_POINT_HPP_INCLUDED
