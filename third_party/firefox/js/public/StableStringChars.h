/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_StableStringChars_h
#define js_StableStringChars_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_INIT_OUTSIDE_CTOR, MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe
#include "mozilla/Range.h"       // mozilla::Range

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/AllocPolicy.h"
#include "js/RootingAPI.h"  // JS::Handle, JS::Rooted
#include "js/String.h"      // JS::GetStringLength
#include "js/TypeDecls.h"   // JSContext, JS::Latin1Char, JSString
#include "js/Vector.h"      // js::Vector

class JSLinearString;

namespace JS {

class MOZ_STACK_CLASS JS_PUBLIC_API AutoStableStringChars final {
  static const size_t InlineCapacity = 24;

  Rooted<JSString*> s_;
  union MOZ_INIT_OUTSIDE_CTOR {
    const char16_t* twoByteChars_;
    const Latin1Char* latin1Chars_;
  };
  MOZ_INIT_OUTSIDE_CTOR uint32_t length_;
  mozilla::Maybe<js::Vector<uint8_t, InlineCapacity>> ownChars_;
  enum State { Uninitialized, Latin1, TwoByte };
  State state_;

  void holdStableChars(JSLinearString* s);

 public:
  explicit AutoStableStringChars(JSContext* cx)
      : s_(cx), state_(Uninitialized) {}

  [[nodiscard]] bool init(JSContext* cx, JSString* s);

  [[nodiscard]] bool initTwoByte(JSContext* cx, JSString* s);

  bool isLatin1() const { return state_ == Latin1; }
  bool isTwoByte() const { return state_ == TwoByte; }

  const Latin1Char* latin1Chars() const {
    MOZ_ASSERT(state_ == Latin1);
    return latin1Chars_;
  }
  const char16_t* twoByteChars() const {
    MOZ_ASSERT(state_ == TwoByte);
    return twoByteChars_;
  }

  mozilla::Range<const Latin1Char> latin1Range() const {
    MOZ_ASSERT(state_ == Latin1);
    return mozilla::Range<const Latin1Char>(latin1Chars_, length());
  }

  mozilla::Range<const char16_t> twoByteRange() const {
    MOZ_ASSERT(state_ == TwoByte);
    return mozilla::Range<const char16_t>(twoByteChars_, length());
  }

  bool maybeGiveOwnershipToCaller() {
    MOZ_ASSERT(state_ != Uninitialized);
    if (!ownChars_.isSome() || !ownChars_->extractRawBuffer()) {
      return false;
    }
    state_ = Uninitialized;
    ownChars_.reset();
    return true;
  }

  size_t length() const {
    MOZ_ASSERT(state_ != Uninitialized);
    return length_;
  }

 private:
  AutoStableStringChars(const AutoStableStringChars& other) = delete;
  void operator=(const AutoStableStringChars& other) = delete;

  template <typename T>
  T* allocOwnChars(JSContext* cx, size_t count);
  bool copyLatin1Chars(JSContext* cx, JSLinearString* linearString);
  bool copyTwoByteChars(JSContext* cx, JSLinearString* linearString);
  bool copyAndInflateLatin1Chars(JSContext*, JSLinearString* linearString);
};

}  

#endif /* js_StableStringChars_h */
