/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_dom_ScriptDecoding_h
#define mozilla_dom_ScriptDecoding_h

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t

#include <type_traits>  // std::is_same

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/CheckedInt.h"  // mozilla::CheckedInt
#include "mozilla/Encoding.h"    // mozilla::Decoder
#include "mozilla/Span.h"        // mozilla::Span
#include "mozilla/UniquePtr.h"   // mozilla::UniquePtr

namespace mozilla::dom {

template <typename Unit>
struct ScriptDecoding {
  static_assert(std::is_same<Unit, char16_t>::value ||
                    std::is_same<Unit, Utf8Unit>::value,
                "must be either UTF-8 or UTF-16");
};

template <>
struct ScriptDecoding<char16_t> {
  static CheckedInt<size_t> MaxBufferLength(const UniquePtr<Decoder>& aDecoder,
                                            uint32_t aByteLength) {
    return aDecoder->MaxUTF16BufferLength(aByteLength);
  }

  static size_t DecodeInto(const UniquePtr<Decoder>& aDecoder,
                           const Span<const uint8_t>& aSrc,
                           Span<char16_t> aDest, bool aEndOfSource) {
    uint32_t result;
    size_t read;
    size_t written;
    std::tie(result, read, written, std::ignore) =
        aDecoder->DecodeToUTF16(aSrc, aDest, aEndOfSource);
    MOZ_ASSERT(result == kInputEmpty);
    MOZ_ASSERT(read == aSrc.Length());
    MOZ_ASSERT(written <= aDest.Length());

    return written;
  }
};

template <>
struct ScriptDecoding<Utf8Unit> {
  static CheckedInt<size_t> MaxBufferLength(const UniquePtr<Decoder>& aDecoder,
                                            uint32_t aByteLength) {
    return aDecoder->MaxUTF8BufferLength(aByteLength);
  }

  static size_t DecodeInto(const UniquePtr<Decoder>& aDecoder,
                           const Span<const uint8_t>& aSrc,
                           Span<Utf8Unit> aDest, bool aEndOfSource) {
    uint32_t result;
    size_t read;
    size_t written;
    std::tie(result, read, written, std::ignore) =
        aDecoder->DecodeToUTF8(aSrc, AsWritableBytes(aDest), aEndOfSource);
    MOZ_ASSERT(result == kInputEmpty);
    MOZ_ASSERT(read == aSrc.Length());
    MOZ_ASSERT(written <= aDest.Length());

    return written;
  }
};

}  

#endif  // mozilla_dom_ScriptDecoding_h
