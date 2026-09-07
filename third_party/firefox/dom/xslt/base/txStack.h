/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef txStack_h_
#define txStack_h_

#include "nsTArray.h"

class txStack : private nsTArray<void*> {
 public:
  inline void* peek() {
    NS_ASSERTION(!isEmpty(), "peeking at empty stack");
    return !isEmpty() ? ElementAt(Length() - 1) : nullptr;
  }

  inline void push(void* aObject) { AppendElement(aObject); }

  inline void* pop() {
    void* object = nullptr;
    NS_ASSERTION(!isEmpty(), "popping from empty stack");
    if (!isEmpty()) {
      object = PopLastElement();
    }
    return object;
  }

  inline bool isEmpty() { return IsEmpty(); }

  inline int32_t size() { return Length(); }

 private:
  friend class txStackIterator;
};

class txStackIterator {
 public:
  inline explicit txStackIterator(txStack* aStack)
      : mStack(aStack), mPosition(0) {}

  inline bool hasNext() { return (mPosition < mStack->Length()); }

  inline void* next() {
    if (mPosition == mStack->Length()) {
      return nullptr;
    }
    return mStack->ElementAt(mPosition++);
  }

 private:
  txStack* mStack;
  uint32_t mPosition;
};

#endif /* txStack_h_ */
