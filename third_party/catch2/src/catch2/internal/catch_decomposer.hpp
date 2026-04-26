
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_DECOMPOSER_HPP_INCLUDED
#define CATCH_DECOMPOSER_HPP_INCLUDED

#include <catch2/catch_tostring.hpp>
#include <catch2/internal/catch_stringref.hpp>
#include <catch2/internal/catch_compare_traits.hpp>
#include <catch2/internal/catch_test_failure_exception.hpp>
#include <catch2/internal/catch_logical_traits.hpp>
#include <catch2/internal/catch_compiler_capabilities.hpp>

#include <type_traits>
#include <iosfwd>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4389)  // '==' : signed/unsigned mismatch
#pragma warning(disable : 4018)  // more "signed/unsigned mismatch"
#pragma warning(disable : 4312)  // Converting int to T* using reinterpret_cast (issue on x64 platform)
#pragma warning(disable : 4180)  // qualifier applied to function type has no meaning
#pragma warning(disable : 4800)  // Forcing result to true or false
#endif

CATCH_INTERNAL_START_WARNINGS_SUPPRESSION
CATCH_INTERNAL_SUPPRESS_SIGN_COMPARE_WARNINGS
CATCH_INTERNAL_SUPPRESS_NON_VIRTUAL_DTOR_WARNINGS

#if defined(CATCH_CPP20_OR_GREATER) && __has_include(<compare>)
#include <compare>
#if defined(__cpp_lib_three_way_comparison) && __cpp_lib_three_way_comparison >= 201907L
#define CATCH_CONFIG_CPP20_COMPARE_OVERLOADS
#endif
#endif

namespace Catch {

namespace Detail {
template <typename T>
using RemoveCVRef_t = std::remove_cv_t<std::remove_reference_t<T>>;
}

template <typename T>
struct capture_by_value : std::integral_constant<bool, std::is_arithmetic<T>::value || std::is_enum<T>::value> {};

#if defined(CATCH_CONFIG_CPP20_COMPARE_OVERLOADS)
template <>
struct capture_by_value<std::strong_ordering> : std::true_type {};
template <>
struct capture_by_value<std::weak_ordering> : std::true_type {};
template <>
struct capture_by_value<std::partial_ordering> : std::true_type {};
#endif

template <typename T>
struct always_false : std::false_type {};

class ITransientExpression {
    bool m_isBinaryExpression;
    bool m_result;

   protected:
    ~ITransientExpression() = default;

   public:
    constexpr auto isBinaryExpression() const -> bool {
        return m_isBinaryExpression;
    }
    constexpr auto getResult() const -> bool {
        return m_result;
    }
    virtual void streamReconstructedExpression(std::ostream& os) const;

    constexpr ITransientExpression(bool isBinaryExpression, bool result)
        : m_isBinaryExpression(isBinaryExpression), m_result(result) {}

    constexpr ITransientExpression(ITransientExpression const&) = default;
    constexpr ITransientExpression& operator=(ITransientExpression const&) = default;

    friend std::ostream& operator<<(std::ostream& out, ITransientExpression const& expr) {
        expr.streamReconstructedExpression(out);
        return out;
    }
};

void formatReconstructedExpression(std::ostream& os, std::string const& lhs, StringRef op, std::string const& rhs);

template <typename LhsT, typename RhsT>
class BinaryExpr : public ITransientExpression {
    LhsT m_lhs;
    StringRef m_op;
    RhsT m_rhs;

    void streamReconstructedExpression(std::ostream& os) const override {
        formatReconstructedExpression(os, Catch::Detail::stringify(m_lhs), m_op, Catch::Detail::stringify(m_rhs));
    }

   public:
    constexpr BinaryExpr(bool comparisonResult, LhsT lhs, StringRef op, RhsT rhs)
        : ITransientExpression{true, comparisonResult}, m_lhs(lhs), m_op(op), m_rhs(rhs) {}

    template <typename T>
    auto operator&&(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator||(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator==(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator!=(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator>(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator<(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator>=(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename T>
    auto operator<=(T) const -> BinaryExpr<LhsT, RhsT const&> const {
        static_assert(always_false<T>::value,
                      "chained comparisons are not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }
};

template <typename LhsT>
class UnaryExpr : public ITransientExpression {
    LhsT m_lhs;

    void streamReconstructedExpression(std::ostream& os) const override {
        os << Catch::Detail::stringify(m_lhs);
    }

   public:
    explicit constexpr UnaryExpr(LhsT lhs) : ITransientExpression{false, static_cast<bool>(lhs)}, m_lhs(lhs) {}
};

template <typename LhsT>
class ExprLhs {
    LhsT m_lhs;

   public:
    explicit constexpr ExprLhs(LhsT lhs) : m_lhs(lhs) {}

#define CATCH_INTERNAL_DEFINE_EXPRESSION_REF_CAPTURE_OPERATOR(op, condition)    \
    template <typename RhsT>                                                    \
    constexpr friend auto operator op(ExprLhs&& lhs, RhsT&& rhs)                \
        ->std::enable_if_t<condition, BinaryExpr<LhsT, RhsT const&>> {          \
        return {static_cast<bool>(lhs.m_lhs op rhs), lhs.m_lhs, #op##_sr, rhs}; \
    }

#define CATCH_INTERNAL_DEFINE_EXPRESSION_VALUE_CAPTURE_OPERATOR(op, condition)                                        \
    template <typename RhsT>                                                                                          \
    constexpr friend auto operator op(ExprLhs&& lhs, RhsT rhs)->std::enable_if_t<condition, BinaryExpr<LhsT, RhsT>> { \
        return {static_cast<bool>(lhs.m_lhs op rhs), lhs.m_lhs, #op##_sr, rhs};                                       \
    }

#define CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR_OVERLOADS(op, ref_capture_condition, value_capture_condition) \
    CATCH_INTERNAL_DEFINE_EXPRESSION_REF_CAPTURE_OPERATOR(op, ref_capture_condition)                            \
    CATCH_INTERNAL_DEFINE_EXPRESSION_VALUE_CAPTURE_OPERATOR(op, value_capture_condition)

#define CATCH_INTERNAL_DEFINE_EXPRESSION_COMMON_OPERATOR_OVERLOADS(id, op)                             \
    CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR_OVERLOADS(                                               \
        op,                                                                                            \
        (Detail::conjunction<Detail::is_##id##_comparable<LhsT, RhsT>,                                 \
                             Detail::negation<capture_by_value<Detail::RemoveCVRef_t<RhsT>>>>::value), \
        (Detail::conjunction<Detail::is_##id##_comparable<LhsT, RhsT>, capture_by_value<RhsT>>::value))

#define CATCH_INTERNAL_IS_INT(T) std::is_same<T, int>
#define CATCH_INTERNAL_IS_INT_OR_LONG(T) Detail::disjunction<std::is_same<T, int>, std::is_same<T, long>>

#define CATCH_INTERNAL_DEFINE_EXPRESSION_0_COMPARABLE_OVERLOADS(id, op, zero_comparable, valid_zero_type)  \
    template <typename RhsT>                                                                               \
    constexpr friend auto operator op(ExprLhs&& lhs, RhsT rhs)                                             \
        ->std::enable_if_t<Detail::conjunction<Detail::negation<Detail::is_##id##_comparable<LhsT, RhsT>>, \
                                               zero_comparable<LhsT>, valid_zero_type(RhsT)>::value,       \
                           BinaryExpr<LhsT, RhsT>> {                                                       \
        if (rhs != 0) {                                                                                    \
            throw_test_failure_exception();                                                                \
        }                                                                                                  \
        return {static_cast<bool>(lhs.m_lhs op 0), lhs.m_lhs, #op##_sr, rhs};                              \
    }                                                                                                      \
    template <typename RhsT>                                                                               \
    constexpr friend auto operator op(ExprLhs&& lhs, RhsT rhs)                                             \
        ->std::enable_if_t<Detail::conjunction<Detail::negation<Detail::is_##id##_comparable<LhsT, RhsT>>, \
                                               zero_comparable<RhsT>, valid_zero_type(LhsT)>::value,       \
                           BinaryExpr<LhsT, RhsT>> {                                                       \
        if (lhs.m_lhs != 0) {                                                                              \
            throw_test_failure_exception();                                                                \
        }                                                                                                  \
        return {static_cast<bool>(0 op rhs), lhs.m_lhs, #op##_sr, rhs};                                    \
    }

#define CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(id, op, zero_comparable, valid_zero_type) \
    CATCH_INTERNAL_DEFINE_EXPRESSION_COMMON_OPERATOR_OVERLOADS(id, op)                                 \
    CATCH_INTERNAL_DEFINE_EXPRESSION_0_COMPARABLE_OVERLOADS(id, op, zero_comparable, valid_zero_type)

    CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(eq, ==, Detail::is_eq_0_comparable,
                                                         CATCH_INTERNAL_IS_INT_OR_LONG)
    CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(ne, !=, Detail::is_eq_0_comparable,
                                                         CATCH_INTERNAL_IS_INT_OR_LONG)
    CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(lt, <, Detail::is_lt_0_comparable, CATCH_INTERNAL_IS_INT)
    CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(le, <=, Detail::is_le_0_comparable, CATCH_INTERNAL_IS_INT)
    CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(gt, >, Detail::is_gt_0_comparable, CATCH_INTERNAL_IS_INT)
    CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR(ge, >=, Detail::is_ge_0_comparable, CATCH_INTERNAL_IS_INT)

#undef CATCH_INTERNAL_DEFINE_EXPRESSION_COMPARABLE_OPERATOR
#undef CATCH_INTERNAL_DEFINE_EXPRESSION_0_COMPARABLE_OVERLOADS
#undef CATCH_INTERNAL_IS_INT_OR_LONG
#undef CATCH_INTERNAL_IS_INT

#define CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR(op)                                                                \
    CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR_OVERLOADS(op, (!capture_by_value<Detail::RemoveCVRef_t<RhsT>>::value), \
                                                        (capture_by_value<RhsT>::value))

    CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR(|)
    CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR(&)
    CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR(^)

#undef CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR
#undef CATCH_INTERNAL_DEFINE_EXPRESSION_OPERATOR_OVERLOADS
#undef CATCH_INTERNAL_DEFINE_EXPRESSION_VALUE_CAPTURE_OPERATOR
#undef CATCH_INTERNAL_DEFINE_EXPRESSION_REF_CAPTURE_OPERATOR

    template <typename RhsT>
    friend auto operator&&(ExprLhs&&, RhsT&&) -> BinaryExpr<LhsT, RhsT const&> {
        static_assert(always_false<RhsT>::value,
                      "operator&& is not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    template <typename RhsT>
    friend auto operator||(ExprLhs&&, RhsT&&) -> BinaryExpr<LhsT, RhsT const&> {
        static_assert(always_false<RhsT>::value,
                      "operator|| is not supported inside assertions, "
                      "wrap the expression inside parentheses, or decompose it");
    }

    constexpr auto makeUnaryExpr() const -> UnaryExpr<LhsT> {
        return UnaryExpr<LhsT>{m_lhs};
    }
};

struct Decomposer {
    template <typename T, std::enable_if_t<!capture_by_value<Detail::RemoveCVRef_t<T>>::value, int> = 0>
    constexpr friend auto operator<=(Decomposer&&, T&& lhs) -> ExprLhs<T const&> {
        return ExprLhs<const T&>{lhs};
    }

    template <typename T, std::enable_if_t<capture_by_value<T>::value, int> = 0>
    constexpr friend auto operator<=(Decomposer&&, T value) -> ExprLhs<T> {
        return ExprLhs<T>{value};
    }
};

}  // namespace Catch

#ifdef _MSC_VER
#pragma warning(pop)
#endif
CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

#endif  // CATCH_DECOMPOSER_HPP_INCLUDED
