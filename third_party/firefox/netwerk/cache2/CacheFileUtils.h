/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CacheFileUtils_h_
#define CacheFileUtils_h_

#include "nsError.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsTArray.h"
#include "mozilla/Mutex.h"

class nsILoadContextInfo;

namespace mozilla {
namespace net {
namespace CacheFileUtils {

extern const char* kAltDataKey;

already_AddRefed<nsILoadContextInfo> ParseKey(const nsACString& aKey,
                                              nsACString* aIdEnhance = nullptr,
                                              nsACString* aURISpec = nullptr);

void AppendKeyPrefix(nsILoadContextInfo* aInfo, nsACString& _retval);

void AppendTagWithValue(nsACString& aTarget, char const aTag,
                        const nsACString& aValue);

nsresult KeyMatchesLoadContextInfo(const nsACString& aKey,
                                   nsILoadContextInfo* aInfo, bool* _retval);

class ValidityPair {
 public:
  ValidityPair(uint32_t aOffset, uint32_t aLen);

  ValidityPair& operator=(const ValidityPair& aOther) = default;

  bool CanBeMerged(const ValidityPair& aOther) const;

  bool IsInOrFollows(uint32_t aOffset) const;

  bool LessThan(const ValidityPair& aOther) const;

  void Merge(const ValidityPair& aOther);

  uint32_t Offset() const { return mOffset; }
  uint32_t Len() const { return mLen; }

 private:
  uint32_t mOffset;
  uint32_t mLen;
};

class ValidityMap {
 public:
  void Log() const;

  uint32_t Length() const;

  void AddPair(uint32_t aOffset, uint32_t aLen);

  void Clear();

  size_t SizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  ValidityPair& operator[](uint32_t aIdx);

 private:
  nsTArray<ValidityPair> mMap;
};

void FreeBuffer(void* aBuf);

nsresult ParseAlternativeDataInfo(const char* aInfo, int64_t* _offset,
                                  nsACString* _type);

void BuildAlternativeDataInfo(const char* aInfo, int64_t aOffset,
                              nsACString& _retval);

class CacheFileLock final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CacheFileLock)
  CacheFileLock() = default;

  mozilla::Mutex& Lock() MOZ_RETURN_CAPABILITY(mLock) { return mLock; }

 private:
  ~CacheFileLock() = default;

  mozilla::Mutex mLock{"CacheFile.mLock"};
};

}  
}  
}  

#endif
