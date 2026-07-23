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

#include "absl/random/discrete_distribution.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <utility>
#include <vector>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {

std::vector<std::pair<double, size_t>> InitDiscreteDistribution(
    std::vector<double>* probabilities) {
  assert(probabilities);
  assert(!probabilities->empty());

  double sum = std::accumulate(std::begin(*probabilities),
                               std::end(*probabilities), 0.0);
  if (std::fabs(sum - 1.0) > 1e-6) {
    for (double& item : *probabilities) {
      item = item / sum;
    }
  }

  const size_t n = probabilities->size();
  std::vector<std::pair<double, size_t>> q;
  q.reserve(n);

  std::vector<size_t> over;
  std::vector<size_t> under;
  size_t idx = 0;
  for (const double item : *probabilities) {
    assert(item >= 0);
    const double v = item * n;
    q.emplace_back(v, 0);
    if (v < 1.0) {
      under.push_back(idx++);
    } else {
      over.push_back(idx++);
    }
  }
  while (!over.empty() && !under.empty()) {
    auto lo = under.back();
    under.pop_back();
    auto hi = over.back();
    over.pop_back();

    q[lo].second = hi;
    const double r = q[hi].first - (1.0 - q[lo].first);
    q[hi].first = r;
    if (r < 1.0) {
      under.push_back(hi);
    } else {
      over.push_back(hi);
    }
  }

  for (auto i : over) {
    q[i] = {1.0, i};
  }
  for (auto i : under) {
    q[i] = {1.0, i};
  }
  return q;
}

}  
ABSL_NAMESPACE_END
}  
