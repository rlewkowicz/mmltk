/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozjemalloc_stall_h)
#define mozjemalloc_stall_h

#include <optional>



namespace mozilla {

namespace detail {
template <typename T>
constexpr bool is_std_optional = false;
template <typename T>
constexpr bool is_std_optional<std::optional<T>> = true;
}  

struct StallSpecs {
  size_t maxAttempts;
  size_t delayMs;

  template <typename DelayFunc, typename OpFunc>
  auto StallAndRetry(DelayFunc&& aDelayFunc, OpFunc&& aOperation) const
      -> decltype(aOperation()) {
    {
      using detail::is_std_optional;
      static_assert(is_std_optional<decltype(aOperation())>,
                    "aOperation() must return std::optional");

    }

    for (size_t i = 0; i < maxAttempts; ++i) {
      aDelayFunc(delayMs);
      if (const auto opt = aOperation()) {
        return opt;
      }
    }
    return std::nullopt;
  }
};


}  

#endif
