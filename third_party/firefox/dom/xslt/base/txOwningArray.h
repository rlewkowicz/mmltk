/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef txOwningArray_h_
#define txOwningArray_h_


template <class E>
class txOwningArray : public nsTArray<E*> {
 public:
  typedef nsTArray<E*> base_type;
  typedef typename base_type::value_type value_type;

  ~txOwningArray() {
    value_type* iter = base_type::Elements();
    value_type* end = iter + base_type::Length();
    for (; iter < end; ++iter) {
      delete *iter;
    }
  }
};

#endif  // txOwningArray_h_
