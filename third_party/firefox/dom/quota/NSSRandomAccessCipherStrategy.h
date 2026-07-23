/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_NSSRANDOMACCESSCIPHERSTRATEGY_H_
#define DOM_QUOTA_NSSRANDOMACCESSCIPHERSTRATEGY_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "CipherStrategy.h"  // for CipherMode
#include "ScopedNSSTypes.h"
#include "mozilla/Maybe.h"
#include "mozilla/ResultExtensions.h"
#include "mozilla/Span.h"

namespace mozilla::dom::quota {

struct NSSRandomAccessCipherStrategy {
  using KeyType = std::array<uint8_t, 32>;
  using BlockNumberType = uint64_t;

  static constexpr size_t BlockNonceSize = 12;

  static constexpr size_t AuthenticationTagSize = 16;

  static Result<KeyType, nsresult> GenerateKey();

  static nsresult Init();

  struct EncryptionInput {
    const KeyType mMasterKey;
    const BlockNumberType mBlockNumber;
    Span<uint8_t, BlockNonceSize> mNonce;
    Span<const uint8_t> mPlaintext;
    Span<const uint8_t> mAad;
  };

  struct EncryptionOutput {
    Span<uint8_t> mCiphertext;
    Span<uint8_t, AuthenticationTagSize> mTag;
  };

  static nsresult Encrypt(const EncryptionInput& aInput,
                          EncryptionOutput& aOutput);

  struct DecryptionInput {
    const KeyType mMasterKey;
    const BlockNumberType mBlockNumber;
    Span<uint8_t, BlockNonceSize> mNonce;
    Span<const uint8_t> mCiphertext;
    Span<const uint8_t> mAad;
    Span<uint8_t, AuthenticationTagSize> mTag;
  };

  struct DecryptionOutput {
    Span<uint8_t> mPlaintext;
  };

  static nsresult Decrypt(const DecryptionInput& aInput,
                          DecryptionOutput& aOutput);

  static std::array<uint8_t, BlockNonceSize> MakeBlockNonce();

  static Span<const uint8_t> SerializeKey(const KeyType& aKey);

  static Maybe<KeyType> DeserializeKey(Span<const uint8_t> aSerializedKey);

 private:
  static nsresult InitContextWithDerivedKey(UniquePK11Context& aContext,
                                            const KeyType& aKey,
                                            CipherMode aCipherMode);

  template <uint32_t V>
  static nsresult DeriveKey(const KeyType& aKey, BlockNumberType aBlockNumber,
                            KeyType& aDerivedKey);
};

}  

#endif  // DOM_QUOTA_NSSRANDOMACCESSCIPHERSTRATEGY_H_
