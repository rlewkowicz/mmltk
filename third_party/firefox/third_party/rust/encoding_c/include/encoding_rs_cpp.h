// Copyright Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#pragma once

#ifndef encoding_rs_cpp_h_
#define encoding_rs_cpp_h_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include "gsl/gsl"

namespace encoding_rs {
class Encoding;
class Decoder;
class Encoder;
};  

#define ENCODING_RS_ENCODING encoding_rs::Encoding
#define ENCODING_RS_NOT_NULL_CONST_ENCODING_PTR \
  gsl::not_null<const encoding_rs::Encoding*>
#define ENCODING_RS_ENCODER encoding_rs::Encoder
#define ENCODING_RS_DECODER encoding_rs::Decoder

#include "encoding_rs.h"

namespace encoding_rs {

class Decoder final {
 public:
  ~Decoder() {}
  static inline void operator delete(void* decoder) {
    decoder_free(reinterpret_cast<Decoder*>(decoder));
  }

  inline gsl::not_null<const Encoding*> encoding() const {
    return gsl::not_null<const Encoding*>(decoder_encoding(this));
  }

  inline std::optional<size_t> max_utf8_buffer_length(
      size_t byte_length) const {
    size_t val = decoder_max_utf8_buffer_length(this, byte_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::optional<size_t> max_utf8_buffer_length_without_replacement(
      size_t byte_length) const {
    size_t val =
        decoder_max_utf8_buffer_length_without_replacement(this, byte_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> decode_to_utf8(
      gsl::span<const uint8_t> src, gsl::span<uint8_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    bool had_replacements;
    uint32_t result =
        decoder_decode_to_utf8(this, null_to_bogus<const uint8_t>(src.data()),
                               &src_read, null_to_bogus<uint8_t>(dst.data()),
                               &dst_written, last, &had_replacements);
    return {result, src_read, dst_written, had_replacements};
  }

  inline std::tuple<uint32_t, size_t, size_t>
  decode_to_utf8_without_replacement(gsl::span<const uint8_t> src,
                                     gsl::span<uint8_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    uint32_t result = decoder_decode_to_utf8_without_replacement(
        this, null_to_bogus<const uint8_t>(src.data()), &src_read,
        null_to_bogus<uint8_t>(dst.data()), &dst_written, last);
    return {result, src_read, dst_written};
  }

  inline std::optional<size_t> max_utf16_buffer_length(
      size_t byte_length) const {
    size_t val = decoder_max_utf16_buffer_length(this, byte_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> decode_to_utf16(
      gsl::span<const uint8_t> src, gsl::span<char16_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    bool had_replacements;
    uint32_t result =
        decoder_decode_to_utf16(this, null_to_bogus<const uint8_t>(src.data()),
                                &src_read, null_to_bogus<char16_t>(dst.data()),
                                &dst_written, last, &had_replacements);
    return {result, src_read, dst_written, had_replacements};
  }

  inline std::tuple<uint32_t, size_t, size_t>
  decode_to_utf16_without_replacement(gsl::span<const uint8_t> src,
                                      gsl::span<char16_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    uint32_t result = decoder_decode_to_utf16_without_replacement(
        this, null_to_bogus<const uint8_t>(src.data()), &src_read,
        null_to_bogus<char16_t>(dst.data()), &dst_written, last);
    return {result, src_read, dst_written};
  }

  inline std::optional<size_t> latin1_byte_compatible_up_to(
      gsl::span<const uint8_t> buffer) const {
    size_t val = decoder_latin1_byte_compatible_up_to(
        this, null_to_bogus<const uint8_t>(buffer.data()),
        static_cast<size_t>(buffer.size()));
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

 private:
  template <class T>
  static inline T* null_to_bogus(T* ptr) {
    return ptr ? ptr : reinterpret_cast<T*>(alignof(T));
  }

  Decoder() = delete;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

class Encoder final {
 public:
  ~Encoder() {}

  static inline void operator delete(void* encoder) {
    encoder_free(reinterpret_cast<Encoder*>(encoder));
  }

  inline gsl::not_null<const Encoding*> encoding() const {
    return gsl::not_null<const Encoding*>(encoder_encoding(this));
  }

  inline bool has_pending_state() const {
    return encoder_has_pending_state(this);
  }

  inline std::optional<size_t> max_buffer_length_from_utf8_if_no_unmappables(
      size_t byte_length) const {
    size_t val = encoder_max_buffer_length_from_utf8_if_no_unmappables(
        this, byte_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::optional<size_t> max_buffer_length_from_utf8_without_replacement(
      size_t byte_length) const {
    size_t val = encoder_max_buffer_length_from_utf8_without_replacement(
        this, byte_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> encode_from_utf8(
      std::string_view src, gsl::span<uint8_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    bool had_replacements;
    uint32_t result = encoder_encode_from_utf8(
        this,
        null_to_bogus<const uint8_t>(
            reinterpret_cast<const uint8_t*>(src.data())),
        &src_read, null_to_bogus<uint8_t>(dst.data()), &dst_written, last,
        &had_replacements);
    return {result, src_read, dst_written, had_replacements};
  }

  inline std::tuple<uint32_t, size_t, size_t>
  encode_from_utf8_without_replacement(std::string_view src,
                                       gsl::span<uint8_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    uint32_t result = encoder_encode_from_utf8_without_replacement(
        this,
        null_to_bogus<const uint8_t>(
            reinterpret_cast<const uint8_t*>(src.data())),
        &src_read, null_to_bogus<uint8_t>(dst.data()), &dst_written, last);
    return {result, src_read, dst_written};
  }

  inline std::optional<size_t> max_buffer_length_from_utf16_if_no_unmappables(
      size_t u16_length) const {
    size_t val = encoder_max_buffer_length_from_utf16_if_no_unmappables(
        this, u16_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::optional<size_t> max_buffer_length_from_utf16_without_replacement(
      size_t u16_length) const {
    size_t val = encoder_max_buffer_length_from_utf16_without_replacement(
        this, u16_length);
    if (val == SIZE_MAX) {
      return std::nullopt;
    }
    return val;
  }

  inline std::tuple<uint32_t, size_t, size_t, bool> encode_from_utf16(
      std::u16string_view src, gsl::span<uint8_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    bool had_replacements;
    uint32_t result = encoder_encode_from_utf16(
        this, null_to_bogus<const char16_t>(src.data()), &src_read,
        null_to_bogus<uint8_t>(dst.data()), &dst_written, last,
        &had_replacements);
    return {result, src_read, dst_written, had_replacements};
  }

  inline std::tuple<uint32_t, size_t, size_t>
  encode_from_utf16_without_replacement(std::u16string_view src,
                                        gsl::span<uint8_t> dst, bool last) {
    size_t src_read = src.size();
    size_t dst_written = dst.size();
    uint32_t result = encoder_encode_from_utf16_without_replacement(
        this, null_to_bogus<const char16_t>(src.data()), &src_read,
        null_to_bogus<uint8_t>(dst.data()), &dst_written, last);
    return {result, src_read, dst_written};
  }

 private:
  template <class T>
  static inline T* null_to_bogus(T* ptr) {
    return ptr ? ptr : reinterpret_cast<T*>(alignof(T));
  }

  Encoder() = delete;
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
};

/**
 * An encoding as defined in the Encoding Standard
 * (https://encoding.spec.whatwg.org/).
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
 * methods `decode()`, `decode_with_bom_removal()`,
 * `decode_without_bom_handling()`,
 * `decode_without_bom_handling_and_without_replacement()` and
 * `encode()`. Unlike the rest of the API, these methods perform heap
 * allocations. You should the `Decoder` and `Encoder` objects when your input
 * is split into multiple buffers or when you want to control the allocation of
 * the output buffers.
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
 * Instances of `Encoding` can be compared with `==`.
 */
class Encoding final {
 public:
  static inline const Encoding* for_label(gsl::cstring_span<> label) {
    return encoding_for_label(
        null_to_bogus<const uint8_t>(
            reinterpret_cast<const uint8_t*>(label.data())),
        label.length());
  }

  static inline const Encoding* for_label_no_replacement(
      gsl::cstring_span<> label) {
    return encoding_for_label_no_replacement(
        null_to_bogus<const uint8_t>(
            reinterpret_cast<const uint8_t*>(label.data())),
        label.length());
  }

  static inline std::optional<
      std::tuple<gsl::not_null<const Encoding*>, size_t>>
  for_bom(gsl::span<const uint8_t> buffer) {
    size_t len = buffer.size();
    const Encoding* encoding =
        encoding_for_bom(null_to_bogus(buffer.data()), &len);
    if (encoding) {
      return std::make_tuple(gsl::not_null<const Encoding*>(encoding), len);
    }
    return std::nullopt;
  }

  inline std::string name() const {
    std::string name(ENCODING_NAME_MAX_LENGTH, '\0');
    size_t length = encoding_name(this, reinterpret_cast<uint8_t*>(&name[0]));
    name.resize(length);
    return name;
  }

  inline bool can_encode_everything() const {
    return encoding_can_encode_everything(this);
  }

  inline bool is_ascii_compatible() const {
    return encoding_is_ascii_compatible(this);
  }

  inline bool is_single_byte() const { return encoding_is_single_byte(this); }

  inline gsl::not_null<const Encoding*> output_encoding() const {
    return gsl::not_null<const Encoding*>(encoding_output_encoding(this));
  }

  inline std::tuple<std::string, gsl::not_null<const Encoding*>, bool> decode(
      gsl::span<const uint8_t> bytes) const {
    auto opt = Encoding::for_bom(bytes);
    const Encoding* encoding;
    if (opt) {
      size_t bom_length;
      std::tie(encoding, bom_length) = *opt;
      bytes = bytes.subspan(bom_length);
    } else {
      encoding = this;
    }
    auto [str, had_errors] = encoding->decode_without_bom_handling(bytes);
    return {str, gsl::not_null<const Encoding*>(encoding), had_errors};
  }

  inline std::tuple<std::string, bool> decode_with_bom_removal(
      gsl::span<const uint8_t> bytes) const {
    if (this == UTF_8_ENCODING && bytes.size() >= 3 &&
        (gsl::as_bytes(bytes.first<3>()) ==
         gsl::as_bytes(gsl::make_span("\xEF\xBB\xBF")))) {
      bytes = bytes.subspan(3, bytes.size() - 3);
    } else if (this == UTF_16LE_ENCODING && bytes.size() >= 2 &&
               (gsl::as_bytes(bytes.first<2>()) ==
                gsl::as_bytes(gsl::make_span("\xFF\xFE")))) {
      bytes = bytes.subspan(2, bytes.size() - 2);
    } else if (this == UTF_16BE_ENCODING && bytes.size() >= 2 &&
               (gsl::as_bytes(bytes.first<2>()) ==
                gsl::as_bytes(gsl::make_span("\xFE\xFF")))) {
      bytes = bytes.subspan(2, bytes.size() - 2);
    }
    return decode_without_bom_handling(bytes);
  }

  inline std::tuple<std::string, bool> decode_without_bom_handling(
      gsl::span<const uint8_t> bytes) const {
    auto decoder = new_decoder_without_bom_handling();
    auto needed = decoder->max_utf8_buffer_length(bytes.size());
    if (!needed) {
      throw std::overflow_error("Overflow in buffer size computation.");
    }
    std::string string(needed.value(), '\0');
    const auto [result, read, written, had_errors] = decoder->decode_to_utf8(
        bytes,
        gsl::make_span(reinterpret_cast<uint8_t*>(&string[0]), string.size()),
        true);
    assert(read == static_cast<size_t>(bytes.size()));
    assert(written <= static_cast<size_t>(string.size()));
    assert(result == INPUT_EMPTY);
    string.resize(written);
    return {string, had_errors};
  }

  inline std::optional<std::string>
  decode_without_bom_handling_and_without_replacement(
      gsl::span<const uint8_t> bytes) const {
    auto decoder = new_decoder_without_bom_handling();
    auto needed =
        decoder->max_utf8_buffer_length_without_replacement(bytes.size());
    if (!needed) {
      throw std::overflow_error("Overflow in buffer size computation.");
    }
    std::string string(needed.value(), '\0');
    const auto [result, read, written] =
        decoder->decode_to_utf8_without_replacement(
            bytes,
            gsl::make_span(reinterpret_cast<uint8_t*>(&string[0]),
                           string.size()),
            true);
    assert(result != OUTPUT_FULL);
    if (result == INPUT_EMPTY) {
      assert(read == static_cast<size_t>(bytes.size()));
      assert(written <= static_cast<size_t>(string.size()));
      string.resize(written);
      return string;
    }
    return std::nullopt;
  }

  inline std::tuple<std::u16string, gsl::not_null<const Encoding*>, bool>
  decode16(gsl::span<const uint8_t> bytes) const {
    auto opt = Encoding::for_bom(bytes);
    const Encoding* encoding;
    if (opt) {
      size_t bom_length;
      std::tie(encoding, bom_length) = *opt;
      bytes = bytes.subspan(bom_length);
    } else {
      encoding = this;
    }
    auto [str, had_errors] = encoding->decode16_without_bom_handling(bytes);
    return {str, gsl::not_null<const Encoding*>(encoding), had_errors};
  }

  inline std::tuple<std::u16string, bool> decode16_with_bom_removal(
      gsl::span<const uint8_t> bytes) const {
    if (this == UTF_8_ENCODING && bytes.size() >= 3 &&
        (gsl::as_bytes(bytes.first<3>()) ==
         gsl::as_bytes(gsl::make_span("\xEF\xBB\xBF")))) {
      bytes = bytes.subspan(3, bytes.size() - 3);
    } else if (this == UTF_16LE_ENCODING && bytes.size() >= 2 &&
               (gsl::as_bytes(bytes.first<2>()) ==
                gsl::as_bytes(gsl::make_span("\xFF\xFE")))) {
      bytes = bytes.subspan(2, bytes.size() - 2);
    } else if (this == UTF_16BE_ENCODING && bytes.size() >= 2 &&
               (gsl::as_bytes(bytes.first<2>()) ==
                gsl::as_bytes(gsl::make_span("\xFE\xFF")))) {
      bytes = bytes.subspan(2, bytes.size() - 2);
    }
    return decode16_without_bom_handling(bytes);
  }

  inline std::tuple<std::u16string, bool> decode16_without_bom_handling(
      gsl::span<const uint8_t> bytes) const {
    auto decoder = new_decoder_without_bom_handling();
    auto needed = decoder->max_utf16_buffer_length(bytes.size());
    if (!needed) {
      throw std::overflow_error("Overflow in buffer size computation.");
    }
    std::u16string string(needed.value(), '\0');
    const auto [result, read, written, had_errors] = decoder->decode_to_utf16(
        bytes, gsl::make_span(&string[0], string.size()), true);
    assert(read == static_cast<size_t>(bytes.size()));
    assert(written <= static_cast<size_t>(string.size()));
    assert(result == INPUT_EMPTY);
    string.resize(written);
    return {string, had_errors};
  }

  inline std::optional<std::u16string>
  decode16_without_bom_handling_and_without_replacement(
      gsl::span<const uint8_t> bytes) const {
    auto decoder = new_decoder_without_bom_handling();
    auto needed = decoder->max_utf16_buffer_length(bytes.size());
    if (!needed) {
      throw std::overflow_error("Overflow in buffer size computation.");
    }
    std::u16string string(needed.value(), '\0');
    const auto [result, read, written] =
        decoder->decode_to_utf16_without_replacement(
            bytes, gsl::make_span(&string[0], string.size()), true);
    assert(result != OUTPUT_FULL);
    if (result == INPUT_EMPTY) {
      assert(read == static_cast<size_t>(bytes.size()));
      assert(written <= static_cast<size_t>(string.size()));
      string.resize(written);
      return string;
    }
    return std::nullopt;
  }

  inline std::tuple<std::vector<uint8_t>, gsl::not_null<const Encoding*>, bool>
  encode(std::string_view string) const {
    auto output_enc = output_encoding();
    if (output_enc == UTF_8_ENCODING) {
      std::vector<uint8_t> vec(string.size());
      std::memcpy(&vec[0], string.data(), string.size());
    }
    auto encoder = output_enc->new_encoder();
    auto needed =
        encoder->max_buffer_length_from_utf8_if_no_unmappables(string.size());
    if (!needed) {
      throw std::overflow_error("Overflow in buffer size computation.");
    }
    std::vector<uint8_t> vec(needed.value());
    bool total_had_errors = false;
    size_t total_read = 0;
    size_t total_written = 0;
    for (;;) {
      const auto [result, read, written, had_errors] =
          encoder->encode_from_utf8(string.substr(total_read),
                                    gsl::make_span(vec).subspan(total_written),
                                    true);
      total_read += read;
      total_written += written;
      total_had_errors |= had_errors;
      if (result == INPUT_EMPTY) {
        assert(total_read == static_cast<size_t>(string.size()));
        assert(total_written <= static_cast<size_t>(vec.size()));
        vec.resize(total_written);
        return {vec, gsl::not_null<const Encoding*>(output_enc),
                total_had_errors};
      }
      auto needed = encoder->max_buffer_length_from_utf8_if_no_unmappables(
          string.size() - total_read);
      if (!needed) {
        throw std::overflow_error("Overflow in buffer size computation.");
      }
      vec.resize(total_written + needed.value());
    }
  }

  inline std::tuple<std::vector<uint8_t>, gsl::not_null<const Encoding*>, bool>
  encode(std::u16string_view string) const {
    auto output_enc = output_encoding();
    auto encoder = output_enc->new_encoder();
    auto needed =
        encoder->max_buffer_length_from_utf16_if_no_unmappables(string.size());
    if (!needed) {
      throw std::overflow_error("Overflow in buffer size computation.");
    }
    std::vector<uint8_t> vec(needed.value());
    bool total_had_errors = false;
    size_t total_read = 0;
    size_t total_written = 0;
    for (;;) {
      const auto [result, read, written, had_errors] =
          encoder->encode_from_utf16(string.substr(total_read),
                                     gsl::make_span(vec).subspan(total_written),
                                     true);
      total_read += read;
      total_written += written;
      total_had_errors |= had_errors;
      if (result == INPUT_EMPTY) {
        assert(total_read == static_cast<size_t>(string.size()));
        assert(total_written <= static_cast<size_t>(vec.size()));
        vec.resize(total_written);
        return {vec, gsl::not_null<const Encoding*>(output_enc),
                total_had_errors};
      }
      auto needed = encoder->max_buffer_length_from_utf16_if_no_unmappables(
          string.size() - total_read);
      if (!needed) {
        throw std::overflow_error("Overflow in buffer size computation.");
      }
      vec.resize(total_written + needed.value());
    }
  }

  inline std::unique_ptr<Decoder> new_decoder() const {
    return std::unique_ptr<Decoder>(encoding_new_decoder(this));
  }

  inline void new_decoder_into(Decoder& decoder) const {
    encoding_new_decoder_into(this, &decoder);
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
  inline std::unique_ptr<Decoder> new_decoder_with_bom_removal() const {
    return std::unique_ptr<Decoder>(
        encoding_new_decoder_with_bom_removal(this));
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
  inline void new_decoder_with_bom_removal_into(Decoder& decoder) const {
    encoding_new_decoder_with_bom_removal_into(this, &decoder);
  }

  inline std::unique_ptr<Decoder> new_decoder_without_bom_handling() const {
    return std::unique_ptr<Decoder>(
        encoding_new_decoder_without_bom_handling(this));
  }

  inline void new_decoder_without_bom_handling_into(Decoder& decoder) const {
    encoding_new_decoder_without_bom_handling_into(this, &decoder);
  }

  inline std::unique_ptr<Encoder> new_encoder() const {
    return std::unique_ptr<Encoder>(encoding_new_encoder(this));
  }

  inline void new_encoder_into(Encoder& encoder) const {
    encoding_new_encoder_into(this, &encoder);
  }

  static inline size_t utf8_valid_up_to(gsl::span<const uint8_t> buffer) {
    return encoding_utf8_valid_up_to(
        null_to_bogus<const uint8_t>(buffer.data()), buffer.size());
  }

  static inline size_t ascii_valid_up_to(gsl::span<const uint8_t> buffer) {
    return encoding_ascii_valid_up_to(
        null_to_bogus<const uint8_t>(buffer.data()), buffer.size());
  }

  static inline size_t iso_2022_jp_ascii_valid_up_to(
      gsl::span<const uint8_t> buffer) {
    return encoding_iso_2022_jp_ascii_valid_up_to(
        null_to_bogus<const uint8_t>(buffer.data()), buffer.size());
  }

 private:
  template <class T>
  static inline T* null_to_bogus(T* ptr) {
    return ptr ? ptr : reinterpret_cast<T*>(alignof(T));
  }

  Encoding() = delete;
  Encoding(const Encoding&) = delete;
  Encoding& operator=(const Encoding&) = delete;
  ~Encoding() = delete;
};

};  

#endif  // encoding_rs_cpp_h_
