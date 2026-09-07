

#ifndef CATCH_MATCHERS_VECTOR_HPP_INCLUDED
#define CATCH_MATCHERS_VECTOR_HPP_INCLUDED

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>

namespace Catch {
namespace Matchers {

template <typename T, typename Alloc>
class VectorContainsElementMatcher final : public MatcherBase<std::vector<T, Alloc>> {
    T const& m_comparator;

   public:
    VectorContainsElementMatcher(T const& comparator) : m_comparator(comparator) {}

    bool match(std::vector<T, Alloc> const& v) const override {
        for (auto const& el : v) {
            if (el == m_comparator) {
                return true;
            }
        }
        return false;
    }

    std::string describe() const override {
        return "Contains: " + ::Catch::Detail::stringify(m_comparator);
    }
};

namespace Detail {

// The vector matchers below differ only in the describe() prefix and in the
// comparison they run against the matched vector, so both are supplied by a
// comparison policy and everything around them is shared.
//
// A comparison policy provides:
//   static constexpr char const* prefix    - the describe() prefix
//   bool operator()(comparator, v) const   - the comparison itself

struct VectorContainsComparison {
    static constexpr char const* prefix = "Contains: ";

    template <typename Comparator, typename Vector>
    bool operator()(Comparator const& comparator, Vector const& v) const {
        if (comparator.size() > v.size())
            return false;
        for (auto const& wanted : comparator) {
            auto present = false;
            for (const auto& el : v) {
                if (el == wanted) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                return false;
            }
        }
        return true;
    }
};

struct VectorEqualsComparison {
    static constexpr char const* prefix = "Equals: ";

    template <typename Comparator, typename Vector>
    bool operator()(Comparator const& comparator, Vector const& v) const {
        if (comparator.size() != v.size()) {
            return false;
        }
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (!(comparator[i] == v[i])) {
                return false;
            }
        }
        return true;
    }
};

struct VectorApproxComparison {
    static constexpr char const* prefix = "is approx: ";

    mutable Catch::Approx approx = Catch::Approx::custom();

    template <typename Comparator, typename Vector>
    bool operator()(Comparator const& comparator, Vector const& v) const {
        if (comparator.size() != v.size())
            return false;
        for (std::size_t i = 0; i < v.size(); ++i)
            if (comparator[i] != approx(v[i]))
                return false;
        return true;
    }
};

struct VectorUnorderedEqualsComparison {
    static constexpr char const* prefix = "UnorderedEquals: ";

    template <typename Comparator, typename Vector>
    bool operator()(Comparator const& comparator, Vector const& v) const {
        if (comparator.size() != v.size()) {
            return false;
        }
        return std::is_permutation(comparator.begin(), comparator.end(), v.begin());
    }
};

// Holds a reference to the comparator vector, matches with Comparison and
// describes itself as "<Comparison::prefix><stringified comparator>".
template <typename Comparison, typename T, typename AllocComp, typename AllocMatch>
class VectorComparisonMatcher : public MatcherBase<std::vector<T, AllocMatch>> {
   protected:
    std::vector<T, AllocComp> const& m_comparator;
    Comparison m_comparison{};

   public:
    VectorComparisonMatcher(std::vector<T, AllocComp> const& comparator) : m_comparator(comparator) {}

    bool match(std::vector<T, AllocMatch> const& v) const override {
        return m_comparison(m_comparator, v);
    }

    std::string describe() const override {
        return Comparison::prefix + ::Catch::Detail::stringify(m_comparator);
    }
};

}

template <typename T, typename AllocComp, typename AllocMatch>
using ContainsMatcher = Detail::VectorComparisonMatcher<Detail::VectorContainsComparison, T, AllocComp, AllocMatch>;

template <typename T, typename AllocComp, typename AllocMatch>
using EqualsMatcher = Detail::VectorComparisonMatcher<Detail::VectorEqualsComparison, T, AllocComp, AllocMatch>;

template <typename T, typename AllocComp, typename AllocMatch>
using UnorderedEqualsMatcher =
    Detail::VectorComparisonMatcher<Detail::VectorUnorderedEqualsComparison, T, AllocComp, AllocMatch>;

// ApproxMatcher is the one member of the family with extra state to tune, so
// it extends the shared matcher instead of being a plain alias for it.
template <typename T, typename AllocComp, typename AllocMatch>
class ApproxMatcher final
    : public Detail::VectorComparisonMatcher<Detail::VectorApproxComparison, T, AllocComp, AllocMatch> {
    using Base = Detail::VectorComparisonMatcher<Detail::VectorApproxComparison, T, AllocComp, AllocMatch>;

   public:
    using Base::Base;

    template <typename = std::enable_if_t<std::is_constructible<double, T>::value>>
    ApproxMatcher& epsilon(T const& newEpsilon) {
        this->m_comparison.approx.epsilon(static_cast<double>(newEpsilon));
        return *this;
    }
    template <typename = std::enable_if_t<std::is_constructible<double, T>::value>>
    ApproxMatcher& margin(T const& newMargin) {
        this->m_comparison.approx.margin(static_cast<double>(newMargin));
        return *this;
    }
    template <typename = std::enable_if_t<std::is_constructible<double, T>::value>>
    ApproxMatcher& scale(T const& newScale) {
        this->m_comparison.approx.scale(static_cast<double>(newScale));
        return *this;
    }
};

template <typename T, typename AllocComp = std::allocator<T>, typename AllocMatch = AllocComp>
ContainsMatcher<T, AllocComp, AllocMatch> Contains(std::vector<T, AllocComp> const& comparator) {
    return ContainsMatcher<T, AllocComp, AllocMatch>(comparator);
}

template <typename T, typename Alloc = std::allocator<T>>
VectorContainsElementMatcher<T, Alloc> VectorContains(T const& comparator) {
    return VectorContainsElementMatcher<T, Alloc>(comparator);
}

template <typename T, typename AllocComp = std::allocator<T>, typename AllocMatch = AllocComp>
EqualsMatcher<T, AllocComp, AllocMatch> Equals(std::vector<T, AllocComp> const& comparator) {
    return EqualsMatcher<T, AllocComp, AllocMatch>(comparator);
}

template <typename T, typename AllocComp = std::allocator<T>, typename AllocMatch = AllocComp>
ApproxMatcher<T, AllocComp, AllocMatch> Approx(std::vector<T, AllocComp> const& comparator) {
    return ApproxMatcher<T, AllocComp, AllocMatch>(comparator);
}

template <typename T, typename AllocComp = std::allocator<T>, typename AllocMatch = AllocComp>
UnorderedEqualsMatcher<T, AllocComp, AllocMatch> UnorderedEquals(std::vector<T, AllocComp> const& target) {
    return UnorderedEqualsMatcher<T, AllocComp, AllocMatch>(target);
}

}
}

#endif  // CATCH_MATCHERS_VECTOR_HPP_INCLUDED
