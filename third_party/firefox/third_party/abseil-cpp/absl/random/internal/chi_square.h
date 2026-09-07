// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_RANDOM_INTERNAL_CHI_SQUARE_H_
#define ABSL_RANDOM_INTERNAL_CHI_SQUARE_H_


#include <cassert>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {

constexpr const char kChiSquared[] = "chi-squared";

template <typename Iterator>
double ChiSquareWithExpected(Iterator begin, Iterator end, double expected) {
  assert(expected >= 10);  
  double chi_square = 0;
  for (auto it = begin; it != end; it++) {
    double d = static_cast<double>(*it) - expected;
    chi_square += d * d;
  }
  chi_square = chi_square / expected;
  return chi_square;
}

template <typename Iterator, typename Expected>
double ChiSquare(Iterator it, Iterator end, Expected eit, Expected eend) {
  double chi_square = 0;
  for (; it != end && eit != eend; ++it, ++eit) {
    if (*it > 0) {
      assert(*eit > 0);
    }
    double e = static_cast<double>(*eit);
    double d = static_cast<double>(*it - *eit);
    if (d != 0) {
      assert(e > 0);
      chi_square += (d * d) / e;
    }
  }
  assert(it == end && eit == eend);
  return chi_square;
}


double ChiSquareValue(int dof, double p);

double ChiSquarePValue(double chi_square, int dof);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_RANDOM_INTERNAL_CHI_SQUARE_H_
