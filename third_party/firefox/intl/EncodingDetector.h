// Copyright 2019 Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


#ifndef mozilla_EncodingDetector_h
#define mozilla_EncodingDetector_h

#include "mozilla/Encoding.h"

namespace mozilla {
class EncodingDetector;
};  

#define CHARDETNG_ENCODING_DETECTOR mozilla::EncodingDetector

#include "chardetng.h"

namespace mozilla {

class EncodingDetector final {
 public:
  ~EncodingDetector() = default;

  static void operator delete(void* aDetector) {
    chardetng_encoding_detector_free(
        reinterpret_cast<EncodingDetector*>(aDetector));
  }

  static inline UniquePtr<EncodingDetector> Create(bool aAllowISO2022JP) {
    UniquePtr<EncodingDetector> detector(
        chardetng_encoding_detector_new(aAllowISO2022JP));
    return detector;
  }

  static inline bool TldMayAffectGuess(Span<const char> aTLD) {
    return chardetng_encoding_detector_tld_may_affect_guess(aTLD.Elements(),
                                                            aTLD.Length());
  }

  inline bool Feed(Span<const uint8_t> aBuffer, bool aLast) {
    return chardetng_encoding_detector_feed(this, aBuffer.Elements(),
                                            aBuffer.Length(), aLast);
  }

  inline mozilla::NotNull<const mozilla::Encoding*> Guess(
      Span<const char> aTLD, bool aAllowUTF8) const {
    return WrapNotNull(chardetng_encoding_detector_guess(
        this, aTLD.Elements(), aTLD.Length(), aAllowUTF8));
  }

 private:
  EncodingDetector() = delete;
  EncodingDetector(const EncodingDetector&) = delete;
  EncodingDetector& operator=(const EncodingDetector&) = delete;
};

};  

#endif  // mozilla_EncodingDetector_h
