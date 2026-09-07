/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(Utils_h)
#define Utils_h

#include <cstring>
#include <type_traits>
#include <limits.h>


#include "mozilla/CheckedInt.h"
#include "mozilla/MathAlgorithms.h"

constexpr size_t LOG2(size_t N) { return mozilla::CeilingLog2(N); }

enum class Order {
  eLess = -1,
  eEqual = 0,
  eGreater = 1,
};

template <typename T>
Order CompareInt(T aValue1, T aValue2) {
  static_assert(std::is_integral_v<T>, "Type must be integral");
  if (aValue1 < aValue2) {
    return Order::eLess;
  }
  if (aValue1 > aValue2) {
    return Order::eGreater;
  }
  return Order::eEqual;
}

template <typename T>
Order CompareAddr(T* aAddr1, T* aAddr2) {
  return CompareInt(uintptr_t(aAddr1), uintptr_t(aAddr2));
}

class Fraction {
 public:
  explicit constexpr Fraction(size_t aNumerator, size_t aDenominator)
      : mNumerator(aNumerator), mDenominator(aDenominator) {}

  MOZ_IMPLICIT constexpr Fraction(long double aValue)
      : mNumerator(aValue * 4096), mDenominator(4096) {}

  inline bool operator<(const Fraction& aOther) const {
#if !defined(MOZ_DEBUG)
    return mNumerator * aOther.mDenominator < aOther.mNumerator * mDenominator;
#else
    mozilla::CheckedInt<size_t> numerator(mNumerator);
    mozilla::CheckedInt<size_t> denominator(mDenominator);
    size_t lhs = (numerator * aOther.mDenominator).value();
    size_t rhs = (aOther.mNumerator * denominator).value();
    return lhs < rhs;
#endif
  }

  inline bool operator>(const Fraction& aOther) const { return aOther < *this; }

  inline bool operator>=(const Fraction& aOther) const {
    return !(*this < aOther);
  }

  inline bool operator<=(const Fraction& aOther) const {
    return !(*this > aOther);
  }

  inline bool operator==(const Fraction& aOther) const {
#if !defined(MOZ_DEBUG)
    return mNumerator * aOther.mDenominator == aOther.mNumerator * mDenominator;
#else
    mozilla::CheckedInt<size_t> numerator(mNumerator);
    mozilla::CheckedInt<size_t> denominator(mDenominator);
    size_t lhs = (numerator * aOther.mDenominator).value();
    size_t rhs = (aOther.mNumerator * denominator).value();
    return lhs == rhs;
#endif
  }

  inline bool operator!=(const Fraction& aOther) const {
    return !(*this == aOther);
  }

 private:
  size_t mNumerator;
  size_t mDenominator;
};

template <typename T>
class FastDivisor {
 private:
  static const unsigned p = 17;

  T m;

 public:
  FastDivisor() : m(0) {}

  FastDivisor(unsigned div, unsigned max) {
    MOZ_ASSERT(div <= max);

    MOZ_ASSERT((1U << p) >= div);

    unsigned m_ = ((1U << p) + div - 1 - (((1U << p) - 1) % div)) / div;

    MOZ_DIAGNOSTIC_ASSERT(max < UINT_MAX / m_);

    MOZ_ASSERT(m_ <= std::numeric_limits<T>::max());
    m = static_cast<T>(m_);

    MOZ_ASSERT(m);

#if defined(MOZ_DEBUG)
    for (unsigned num = 0; num < max; num += div) {
      MOZ_ASSERT(num / div == divide(num));
    }
#endif
  }

  inline uint32_t divide(uint32_t num) const {
    MOZ_ASSERT(m);
    return (num * m) >> p;
  }
};

template <typename T>
unsigned inline operator/(unsigned num, FastDivisor<T> divisor) {
  return divisor.divide(num);
}

#define ALIGNMENT_ADDR2OFFSET(a, alignment) \
  ((size_t)((uintptr_t)(a) & ((alignment) - 1)))

#define ALIGNMENT_CEILING(s, alignment) \
  (((s) + ((alignment) - 1)) & (~((alignment) - 1)))

#define ALIGNMENT_FLOOR(s, alignment) ((s) & (~((alignment) - 1)))

static inline const char* _getprogname(void) { return "<jemalloc>"; }

#  define _write write
inline void _malloc_message(const char* p) {
  if (_write(STDERR_FILENO, p, (unsigned int)strlen(p)) < 0) {
    return;
  }
}

template <typename... Args>
static void _malloc_message(const char* p, Args... args) {
  _malloc_message(p);
  _malloc_message(args...);
}

#endif
