/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TypedIndex_h
#define frontend_TypedIndex_h

#include <compare>  // std::strong_ordering
#include <cstdint>
#include <stddef.h>

namespace js {
namespace frontend {

template <typename Tag>
struct TypedIndex {
  TypedIndex() = default;
  constexpr explicit TypedIndex(uint32_t index) : index(index) {};

  uint32_t index = 0;

  operator size_t() const { return index; }

  TypedIndex& operator=(size_t idx) {
    index = idx;
    return *this;
  }

  constexpr auto operator<=>(const TypedIndex& other) const = default;
};

}  
}  

#endif
