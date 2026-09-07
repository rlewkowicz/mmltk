/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_ENCRYPTEDRANDOMACCESSBLOCKVIEW_H_
#define DOM_QUOTA_ENCRYPTEDRANDOMACCESSBLOCKVIEW_H_

#include <cstdint>
#include <cstring>

#include "EncryptedRandomAccessBlock.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/Span.h"

namespace mozilla::dom::quota {


class EncryptedRandomAccessBlockCipherMetadataViewV1 {
 public:
  static constexpr size_t NonceSize = 12;
  static constexpr size_t AuthenticationTagSize = 16;

  template <size_t N>
  using ConstSpan = Span<const uint8_t, N>;
  template <size_t N>
  using MutableSpan = Span<uint8_t, N>;

 private:
  static constexpr size_t ReservedBytesSize = 4;

  static_assert(NonceSize + AuthenticationTagSize + ReservedBytesSize ==
                EncryptedRandomAccessBlock::CipherMetadataSize);

 public:
  explicit EncryptedRandomAccessBlockCipherMetadataViewV1(
      MutableSpan<NonceSize + AuthenticationTagSize + ReservedBytesSize> aBlock)
      : mCipherMetadata(aBlock) {}

  ConstSpan<NonceSize> Nonce() const {
    return mCipherMetadata.First<NonceSize>();
  }

  MutableSpan<NonceSize> MutableNonce() {
    return mCipherMetadata.First<NonceSize>();
  }

  void SetNonce(ConstSpan<NonceSize> aNonce) {
    memcpy(mCipherMetadata.data(), aNonce.data(), NonceSize);
  }

  ConstSpan<AuthenticationTagSize> AuthenticationTag() const {
    return mCipherMetadata.Subspan<NonceSize, AuthenticationTagSize>();
  }

  MutableSpan<AuthenticationTagSize> MutableAuthenticationTag() {
    return mCipherMetadata.Subspan<NonceSize, AuthenticationTagSize>();
  }

  void SetAuthenticationTag(
      ConstSpan<AuthenticationTagSize> aAuthenticationTag) {
    memcpy(mCipherMetadata.data() + NonceSize, aAuthenticationTag.data(),
           AuthenticationTagSize);
  }

 private:
  MutableSpan<NonceSize + AuthenticationTagSize + ReservedBytesSize>
      mCipherMetadata;
};

class DecryptedRandomAccessBlockCipherPayloadView {
 public:
  using TextLengthType = uint16_t;
  static constexpr size_t TextLengthFieldSize = sizeof(TextLengthType);
  static_assert(TextLengthFieldSize == 2,
                "TextLength should take 2 bytes on disk.");

  static constexpr TextLengthType MaxTextLength =
      EncryptedRandomAccessBlock::CipherPayloadSize - TextLengthFieldSize;
  static_assert(MaxTextLength == 4030, "MaxTextLength should be 4030 bytes.");

  template <size_t N>
  using ConstSpan = Span<const uint8_t, N>;
  template <size_t N>
  using MutableSpan = Span<uint8_t, N>;

  explicit DecryptedRandomAccessBlockCipherPayloadView(
      MutableSpan<TextLengthFieldSize + MaxTextLength> aDecryptedCipherPayload)
      : mDecryptedCipherPayload(aDecryptedCipherPayload) {}

  TextLengthType TextLength() const {
    return mozilla::LittleEndian::readUint16(mDecryptedCipherPayload.data());
  }

  void SetTextLength(TextLengthType aLength) {
    mozilla::LittleEndian::writeUint16(mDecryptedCipherPayload.data(), aLength);
  }

  ConstSpan<MaxTextLength> TextAndPadding() const {
    return mDecryptedCipherPayload
        .Subspan<TextLengthFieldSize, MaxTextLength>();
  }

  MutableSpan<MaxTextLength> MutableTextAndPadding() {
    return mDecryptedCipherPayload
        .Subspan<TextLengthFieldSize, MaxTextLength>();
  }

 private:
  MutableSpan<TextLengthFieldSize + MaxTextLength> mDecryptedCipherPayload;
};

}  

#endif  // DOM_QUOTA_ENCRYPTEDRANDOMACCESSBLOCKVIEW_H_
