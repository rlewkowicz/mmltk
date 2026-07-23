// Copyright 2015-2016 Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.


#ifndef mozilla_Encoding_h
#define mozilla_Encoding_h

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/NotNull.h"
#include "mozilla/Span.h"
#include "nsString.h"

#include <tuple>

namespace mozilla {
class Encoding;
class Decoder;
class Encoder;
};  

#define ENCODING_RS_ENCODING mozilla::Encoding
#define ENCODING_RS_NOT_NULL_CONST_ENCODING_PTR \
  mozilla::NotNull<const mozilla::Encoding*>
#define ENCODING_RS_ENCODER mozilla::Encoder
#define ENCODING_RS_DECODER mozilla::Decoder

#include "encoding_rs.h"

extern "C" {

nsresult mozilla_encoding_decode_to_nsstring(mozilla::Encoding const** encoding,
                                             uint8_t const* src, size_t src_len,
                                             nsAString* dst);

nsresult mozilla_encoding_decode_to_nsstring_with_bom_removal(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsAString* dst);

nsresult mozilla_encoding_decode_to_nsstring_without_bom_handling(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsAString* dst);

nsresult
mozilla_encoding_decode_to_nsstring_without_bom_handling_and_without_replacement(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsAString* dst);

nsresult mozilla_encoding_encode_from_utf16(mozilla::Encoding const** encoding,
                                            char16_t const* src, size_t src_len,
                                            nsACString* dst);

nsresult mozilla_encoding_decode_to_nscstring(
    mozilla::Encoding const** encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_decode_to_nscstring_with_bom_removal(
    mozilla::Encoding const* encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_decode_to_nscstring_without_bom_handling(
    mozilla::Encoding const* encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_decode_from_slice_to_nscstring_without_bom_handling(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsACString* dst, size_t already_validated);

nsresult
mozilla_encoding_decode_to_nscstring_without_bom_handling_and_without_replacement(
    mozilla::Encoding const* encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_encode_from_nscstring(
    mozilla::Encoding const** encoding, nsACString const* src, nsACString* dst);

}  

namespace mozilla {

const uint32_t kInputEmpty = INPUT_EMPTY;

const uint32_t kOutputFull = OUTPUT_FULL;

/**
 * An encoding as defined in the Encoding Standard
 * (https://encoding.spec.whatwg.org/).
 *
 * See https://docs.rs/encoding_rs/ for the Rust API docs.
 *
 * An _encoding_ defines a mapping from a byte sequence to a Unicode code point
 * sequence and, in most cases, vice versa. Each encoding has a name, an output
 * encoding, and one or more labels.
 *
 * _Labels_ are ASCII-case-insensitive strings that are used to identify an
 * encoding in formats and protocols. The _name_ of the encoding is the
 * preferred label in the case appropriate for returning from the
 * `characterSet` property of the `Document` DOM interface, except for
 * the replacement encoding whose name is not one of its labels.
 *
 * The _output encoding_ is the encoding used for form submission and URL
 * parsing on Web pages in the encoding. This is UTF-8 for the replacement,
 * UTF-16LE and UTF-16BE encodings and the encoding itself for other
 * encodings.
 *
 * # Streaming vs. Non-Streaming
 *
 * When you have the entire input in a single buffer, you can use the
 * methods `Decode()`, `DecodeWithBOMRemoval()`,
 * `DecodeWithoutBOMHandling()`,
 * `DecodeWithoutBOMHandlingAndWithoutReplacement()` and
 * `Encode()`. Unlike the rest of the API (apart from the `NewDecoder()` and
 * NewEncoder()` methods), these methods perform heap allocations. You should
 * the `Decoder` and `Encoder` objects when your input is split into multiple
 * buffers or when you want to control the allocation of the output buffers.
 *
 * # Instances
 *
 * All instances of `Encoding` are statically allocated and have the process's
 * lifetime. There is precisely one unique `Encoding` instance for each
 * encoding defined in the Encoding Standard.
 *
 * To obtain a reference to a particular encoding whose identity you know at
 * compile time, use a `static` that refers to encoding. There is a `static`
 * for each encoding. The `static`s are named in all caps with hyphens
 * replaced with underscores and with `_ENCODING` appended to the
 * name. For example, if you know at compile time that you will want to
 * decode using the UTF-8 encoding, use the `UTF_8_ENCODING` `static`.
 *
 * If you don't know what encoding you need at compile time and need to
 * dynamically get an encoding by label, use `Encoding::for_label()`.
 *
 * Pointers to `Encoding` can be compared with `==` to check for the sameness
 * of two encodings.
 *
 * A pointer to a `mozilla::Encoding` in C++ is the same thing as a pointer
 * to an `encoding_rs::Encoding` in Rust. When writing FFI code, use
 * `const mozilla::Encoding*` in the C signature and
 * `*const encoding_rs::Encoding` is the corresponding Rust signature.
 */
class Encoding final {
 public:
  static inline const Encoding* ForLabel(Span<const char> aLabel) {
    return encoding_for_label(
        reinterpret_cast<const uint8_t*>(aLabel.Elements()), aLabel.Length());
  }

  static inline const Encoding* ForLabel(const nsAString& aLabel) {
    return Encoding::ForLabel(NS_ConvertUTF16toUTF8(aLabel));
  }

  static inline const Encoding* ForLabelNoReplacement(Span<const char> aLabel) {
    return encoding_for_label_no_replacement(
        reinterpret_cast<const uint8_t*>(aLabel.Elements()), aLabel.Length());
  }

  static inline const Encoding* ForLabelNoReplacement(const nsAString& aLabel) {
    return Encoding::ForLabelNoReplacement(NS_ConvertUTF16toUTF8(aLabel));
  }

  static inline std::tuple<const Encoding*, size_t> ForBOM(
      Span<const uint8_t> aBuffer) {
    size_t len = aBuffer.Length();
    const Encoding* encoding = encoding_for_bom(aBuffer.Elements(), &len);
    return {encoding, len};
  }

  inline void Name(nsACString& aName) const {
    aName.SetLength(ENCODING_NAME_MAX_LENGTH);
    size_t length =
        encoding_name(this, reinterpret_cast<uint8_t*>(aName.BeginWriting()));
    aName.SetLength(length);  
  }

  inline bool CanEncodeEverything() const {
    return encoding_can_encode_everything(this);
  }

  inline bool IsSingleByte() const { return encoding_is_single_byte(this); }

  inline bool IsAsciiCompatible() const {
    return encoding_is_ascii_compatible(this);
  }

  inline NotNull<const mozilla::Encoding*> OutputEncoding() const {
    return WrapNotNull(encoding_output_encoding(this));
  }

  inline std::tuple<nsresult, NotNull<const mozilla::Encoding*>> Decode(
      const nsACString& aBytes, nsACString& aOut) const {
    const Encoding* encoding = this;
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    nsresult rv;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      rv = mozilla_encoding_decode_to_nscstring(&encoding, &temp, out);
    } else {
      rv = mozilla_encoding_decode_to_nscstring(&encoding, bytes, out);
    }
    return {rv, WrapNotNull(encoding)};
  }

  inline std::tuple<nsresult, NotNull<const mozilla::Encoding*>> Decode(
      Span<const uint8_t> aBytes, nsAString& aOut) const {
    const Encoding* encoding = this;
    nsresult rv = mozilla_encoding_decode_to_nsstring(
        &encoding, aBytes.Elements(), aBytes.Length(), &aOut);
    return {rv, WrapNotNull(encoding)};
  }

  inline nsresult DecodeWithBOMRemoval(const nsACString& aBytes,
                                       nsACString& aOut) const {
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      return mozilla_encoding_decode_to_nscstring_with_bom_removal(this, &temp,
                                                                   out);
    }
    return mozilla_encoding_decode_to_nscstring_with_bom_removal(this, bytes,
                                                                 out);
  }

  inline nsresult DecodeWithBOMRemoval(Span<const uint8_t> aBytes,
                                       nsAString& aOut) const {
    return mozilla_encoding_decode_to_nsstring_with_bom_removal(
        this, aBytes.Elements(), aBytes.Length(), &aOut);
  }

  inline nsresult DecodeWithoutBOMHandling(const nsACString& aBytes,
                                           nsACString& aOut) const {
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      return mozilla_encoding_decode_to_nscstring_without_bom_handling(
          this, &temp, out);
    }
    return mozilla_encoding_decode_to_nscstring_without_bom_handling(
        this, bytes, out);
  }

  inline nsresult DecodeWithoutBOMHandling(Span<const uint8_t> aBytes,
                                           nsAString& aOut) const {
    return mozilla_encoding_decode_to_nsstring_without_bom_handling(
        this, aBytes.Elements(), aBytes.Length(), &aOut);
  }

  inline nsresult DecodeWithoutBOMHandlingAndWithoutReplacement(
      const nsACString& aBytes, nsACString& aOut) const {
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      return mozilla_encoding_decode_to_nscstring_without_bom_handling_and_without_replacement(
          this, &temp, out);
    }
    return mozilla_encoding_decode_to_nscstring_without_bom_handling_and_without_replacement(
        this, bytes, out);
  }

  inline nsresult DecodeWithoutBOMHandling(Span<const uint8_t> aBytes,
                                           nsACString& aOut,
                                           size_t aAlreadyValidated) const {
    return mozilla_encoding_decode_from_slice_to_nscstring_without_bom_handling(
        this, aBytes.Elements(), aBytes.Length(), &aOut, aAlreadyValidated);
  }

  inline nsresult DecodeWithoutBOMHandlingAndWithoutReplacement(
      Span<const uint8_t> aBytes, nsAString& aOut) const {
    return mozilla_encoding_decode_to_nsstring_without_bom_handling_and_without_replacement(
        this, aBytes.Elements(), aBytes.Length(), &aOut);
  }

  inline std::tuple<nsresult, NotNull<const mozilla::Encoding*>> Encode(
      const nsACString& aString, nsACString& aOut) const {
    const Encoding* encoding = this;
    const nsACString* string = &aString;
    nsACString* out = &aOut;
    nsresult rv;
    if (string == out) {
      nsAutoCString temp(aString);
      rv = mozilla_encoding_encode_from_nscstring(&encoding, &temp, out);
    } else {
      rv = mozilla_encoding_encode_from_nscstring(&encoding, string, out);
    }
    return {rv, WrapNotNull(encoding)};
  }

  inline std::tuple<nsresult, NotNull<const mozilla::Encoding*>> Encode(
      Span<const char16_t> aString, nsACString& aOut) const {
    const Encoding* encoding = this;
    nsresult rv = mozilla_encoding_encode_from_utf16(
        &encoding, aString.Elements(), aString.Length(), &aOut);
    return {rv, WrapNotNull(encoding)};
  }

  inline UniquePtr<Decoder> NewDecoder() const {
    UniquePtr<Decoder> decoder(encoding_new_decoder(this));
    return decoder;
  }

  inline void NewDecoderInto(Decoder& aDecoder) const {
    encoding_new_decoder_into(this, &aDecoder);
  }

  /**
   * Instantiates a new decoder for this encoding with BOM removal.
   *
   * If the input starts with bytes that are the BOM for this encoding,
   * those bytes are removed. However, the decoder never morphs into a
   * decoder for another encoding: A BOM for another encoding is treated as
   * (potentially malformed) input to the decoding algorithm for this
   * encoding.
   */
  inline UniquePtr<Decoder> NewDecoderWithBOMRemoval() const {
    UniquePtr<Decoder> decoder(encoding_new_decoder_with_bom_removal(this));
    return decoder;
  }

  /**
   * Instantiates a new decoder for this encoding with BOM removal
   * into memory occupied by a previously-instantiated decoder.
   *
   * If the input starts with bytes that are the BOM for this encoding,
   * those bytes are removed. However, the decoder never morphs into a
   * decoder for another encoding: A BOM for another encoding is treated as
   * (potentially malformed) input to the decoding algorithm for this
   * encoding.
   */
  inline void NewDecoderWithBOMRemovalInto(Decoder& aDecoder) const {
    encoding_new_decoder_with_bom_removal_into(this, &aDecoder);
  }

  inline UniquePtr<Decoder> NewDecoderWithoutBOMHandling() const {
    UniquePtr<Decoder> decoder(encoding_new_decoder_without_bom_handling(this));
    return decoder;
  }

  inline void NewDecoderWithoutBOMHandlingInto(Decoder& aDecoder) const {
    encoding_new_decoder_without_bom_handling_into(this, &aDecoder);
  }

  inline UniquePtr<Encoder> NewEncoder() const {
    UniquePtr<Encoder> encoder(encoding_new_encoder(this));
    return encoder;
  }

  inline void NewEncoderInto(Encoder& aEncoder) const {
    encoding_new_encoder_into(this, &aEncoder);
  }

  static inline size_t UTF8ValidUpTo(Span<const uint8_t> aBuffer) {
    return encoding_utf8_valid_up_to(aBuffer.Elements(), aBuffer.Length());
  }

  static inline size_t ASCIIValidUpTo(Span<const uint8_t> aBuffer) {
    return encoding_ascii_valid_up_to(aBuffer.Elements(), aBuffer.Length());
  }

  static inline size_t ISO2022JPASCIIValidUpTo(Span<const uint8_t> aBuffer) {
    return encoding_iso_2022_jp_ascii_valid_up_to(aBuffer.Elements(),
                                                  aBuffer.Length());
  }

 private:
  Encoding() = delete;
  Encoding(const Encoding&) = delete;
  Encoding& operator=(const Encoding&) = delete;
  ~Encoding() = delete;
};

class Decoder final {
 public:
  ~Decoder() = default;
  static void operator delete(void* aDecoder) {
    decoder_free(reinterpret_cast<Decoder*>(aDecoder));
  }

  inline NotNull<const mozilla::Encoding*> Encoding() const {
    return WrapNotNull(decoder_encoding(this));
  }

  inline CheckedInt<size_t> MaxUTF8BufferLength(size_t aByteLength) const {
    CheckedInt<size_t> max(decoder_max_utf8_buffer_length(this, aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  inline CheckedInt<size_t> MaxUTF8BufferLengthWithoutReplacement(
      size_t aByteLength) const {
    CheckedInt<size_t> max(
        decoder_max_utf8_buffer_length_without_replacement(this, aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> DecodeToUTF8(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result =
        decoder_decode_to_utf8(this, aSrc.Elements(), &srcRead, aDst.Elements(),
                               &dstWritten, aLast, &hadReplacements);
    return {result, srcRead, dstWritten, hadReplacements};
  }

  inline std::tuple<uint32_t, size_t, size_t> DecodeToUTF8WithoutReplacement(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = decoder_decode_to_utf8_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return {result, srcRead, dstWritten};
  }

  inline CheckedInt<size_t> MaxUTF16BufferLength(size_t aU16Length) const {
    CheckedInt<size_t> max(decoder_max_utf16_buffer_length(this, aU16Length));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> DecodeToUTF16(
      Span<const uint8_t> aSrc, Span<char16_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result = decoder_decode_to_utf16(this, aSrc.Elements(), &srcRead,
                                              aDst.Elements(), &dstWritten,
                                              aLast, &hadReplacements);
    return {result, srcRead, dstWritten, hadReplacements};
  }

  inline std::tuple<uint32_t, size_t, size_t> DecodeToUTF16WithoutReplacement(
      Span<const uint8_t> aSrc, Span<char16_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = decoder_decode_to_utf16_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return {result, srcRead, dstWritten};
  }

  inline mozilla::Maybe<size_t> Latin1ByteCompatibleUpTo(
      Span<const uint8_t> aBuffer) const {
    size_t upTo = decoder_latin1_byte_compatible_up_to(this, aBuffer.Elements(),
                                                       aBuffer.Length());
    if (upTo == std::numeric_limits<size_t>::max()) {
      return mozilla::Nothing();
    }
    return mozilla::Some(upTo);
  }

 private:
  Decoder() = delete;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

class Encoder final {
 public:
  ~Encoder() = default;

  static void operator delete(void* aEncoder) {
    encoder_free(reinterpret_cast<Encoder*>(aEncoder));
  }

  inline NotNull<const mozilla::Encoding*> Encoding() const {
    return WrapNotNull(encoder_encoding(this));
  }

  inline bool HasPendingState() const {
    return encoder_has_pending_state(this);
  }

  inline CheckedInt<size_t> MaxBufferLengthFromUTF8IfNoUnmappables(
      size_t aByteLength) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf8_if_no_unmappables(this,
                                                              aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  inline CheckedInt<size_t> MaxBufferLengthFromUTF8WithoutReplacement(
      size_t aByteLength) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf8_without_replacement(this,
                                                                aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Incrementally encode into byte stream from UTF-8 with unmappable
   * characters replaced with HTML (decimal) numeric character references.
   *
   * See the documentation of the class for documentation for `Encode*`
   * methods collectively.
   *
   * WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING:
   * The input ***MUST*** be valid UTF-8 or bad things happen! Unless
   * absolutely sure, use `Encoding::UTF8ValidUpTo()` to check.
   */
  inline std::tuple<uint32_t, size_t, size_t, bool> EncodeFromUTF8(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result = encoder_encode_from_utf8(this, aSrc.Elements(), &srcRead,
                                               aDst.Elements(), &dstWritten,
                                               aLast, &hadReplacements);
    return {result, srcRead, dstWritten, hadReplacements};
  }

  /**
   * Incrementally encode into byte stream from UTF-8 _without replacement_.
   *
   * See the documentation of the class for documentation for `Encode*`
   * methods collectively.
   *
   * WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING:
   * The input ***MUST*** be valid UTF-8 or bad things happen! Unless
   * absolutely sure, use `Encoding::UTF8ValidUpTo()` to check.
   */
  inline std::tuple<uint32_t, size_t, size_t> EncodeFromUTF8WithoutReplacement(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = encoder_encode_from_utf8_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return {result, srcRead, dstWritten};
  }

  inline CheckedInt<size_t> MaxBufferLengthFromUTF16IfNoUnmappables(
      size_t aU16Length) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf16_if_no_unmappables(this,
                                                               aU16Length));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  inline CheckedInt<size_t> MaxBufferLengthFromUTF16WithoutReplacement(
      size_t aU16Length) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf16_without_replacement(this,
                                                                 aU16Length));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> EncodeFromUTF16(
      Span<const char16_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result = encoder_encode_from_utf16(this, aSrc.Elements(), &srcRead,
                                                aDst.Elements(), &dstWritten,
                                                aLast, &hadReplacements);
    return {result, srcRead, dstWritten, hadReplacements};
  }

  inline std::tuple<uint32_t, size_t, size_t> EncodeFromUTF16WithoutReplacement(
      Span<const char16_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = encoder_encode_from_utf16_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return {result, srcRead, dstWritten};
  }

 private:
  Encoder() = delete;
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
};

};  

#endif  // mozilla_Encoding_h
