/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CacheLog.h"
#include "CacheFileUtils.h"
#include "CacheObserver.h"
#include "LoadContextInfo.h"
#include "mozilla/Tokenizer.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include <algorithm>

namespace mozilla::net::CacheFileUtils {

static uint32_t const kAltDataVersion = 1;
const char* kAltDataKey = "alt-data";

namespace {

class KeyParser : protected Tokenizer {
 public:
  explicit KeyParser(nsACString const& aInput)
      : Tokenizer(aInput),
        isAnonymous(false)
        ,
        lastTag(0) {}

 private:
  OriginAttributes originAttribs;
  bool isAnonymous;
  nsCString idEnhance;
  nsDependentCSubstring cacheKey;

  char lastTag;

  static bool TagChar(const char aChar) {
    unsigned char c = static_cast<unsigned char>(aChar);
    return c >= ' ' && c <= '\x7f';
  }

  bool ParseTags() {
    if (CheckEOF()) {
      return true;
    }

    char tag;
    if (!ReadChar(&TagChar, &tag)) {
      return false;
    }

    if (!(lastTag < tag || tag == ':')) {
      return false;
    }
    lastTag = tag;

    switch (tag) {
      case ':':
        cacheKey.Rebind(mCursor, mEnd - mCursor);
        return true;
      case 'O': {
        nsAutoCString originSuffix;
        if (!ParseValue(&originSuffix) ||
            !originAttribs.PopulateFromSuffix(originSuffix)) {
          return false;
        }
        break;
      }
      case 'p':
        originAttribs.SyncAttributesWithPrivateBrowsing(true);
        break;
      case 'b':
        break;
      case 'a':
        isAnonymous = true;
        break;
      case 'i': {
        uint32_t deprecatedAppId = 0;
        if (!ReadInteger(&deprecatedAppId)) {
          return false;  
        }
        break;
      }
      case '~':
        if (!ParseValue(&idEnhance)) {
          return false;
        }
        break;
      default:
        if (!ParseValue()) {  
          return false;
        }
        break;
    }

    if (!CheckChar(',')) {
      return false;
    }

    return ParseTags();
  }

  bool ParseValue(nsACString* result = nullptr) {
    if (CheckEOF()) {
      return false;
    }

    Token t;
    while (Next(t)) {
      if (!Token::Char(',').Equals(t)) {
        if (result) {
          result->Append(t.Fragment());
        }
        continue;
      }

      if (CheckChar(',')) {
        if (result) {
          result->Append(',');
        }
        continue;
      }

      Rollback();
      return true;
    }

    return false;
  }

 public:
  already_AddRefed<LoadContextInfo> Parse() {
    RefPtr<LoadContextInfo> info;
    if (ParseTags()) {
      info = GetLoadContextInfo(isAnonymous, originAttribs);
    }

    return info.forget();
  }

  void URISpec(nsACString& result) { result.Assign(cacheKey); }

  void IdEnhance(nsACString& result) { result.Assign(idEnhance); }
};

}  

already_AddRefed<nsILoadContextInfo> ParseKey(const nsACString& aKey,
                                              nsACString* aIdEnhance,
                                              nsACString* aURISpec) {
  KeyParser parser(aKey);
  RefPtr<LoadContextInfo> info = parser.Parse();

  if (info) {
    if (aIdEnhance) parser.IdEnhance(*aIdEnhance);
    if (aURISpec) parser.URISpec(*aURISpec);
  }

  return info.forget();
}

void AppendKeyPrefix(nsILoadContextInfo* aInfo, nsACString& _retval) {

  if (!aInfo) {
    return;
  }

  OriginAttributes const* oa = aInfo->OriginAttributesPtr();
  nsAutoCString suffix;
  oa->CreateSuffix(suffix);
  if (!suffix.IsEmpty()) {
    AppendTagWithValue(_retval, 'O', suffix);
  }

  if (aInfo->IsAnonymous()) {
    _retval.AppendLiteral("a,");
  }

  if (aInfo->IsPrivate()) {
    _retval.AppendLiteral("p,");
  }
}

void AppendTagWithValue(nsACString& aTarget, char const aTag,
                        const nsACString& aValue) {
  aTarget.Append(aTag);

  if (!aValue.IsEmpty()) {
    if (!aValue.Contains(',')) {
      aTarget.Append(aValue);
    } else {
      nsAutoCString escapedValue(aValue);
      escapedValue.ReplaceSubstring(","_ns, ",,"_ns);
      aTarget.Append(escapedValue);
    }
  }

  aTarget.Append(',');
}

nsresult KeyMatchesLoadContextInfo(const nsACString& aKey,
                                   nsILoadContextInfo* aInfo, bool* _retval) {
  nsCOMPtr<nsILoadContextInfo> info = ParseKey(aKey);

  if (!info) {
    return NS_ERROR_FAILURE;
  }

  *_retval = info->Equals(aInfo);
  return NS_OK;
}

ValidityPair::ValidityPair(uint32_t aOffset, uint32_t aLen)
    : mOffset(aOffset), mLen(aLen) {}

bool ValidityPair::CanBeMerged(const ValidityPair& aOther) const {
  return IsInOrFollows(aOther.mOffset) || aOther.IsInOrFollows(mOffset);
}

bool ValidityPair::IsInOrFollows(uint32_t aOffset) const {
  return mOffset <= aOffset && mOffset + mLen >= aOffset;
}

bool ValidityPair::LessThan(const ValidityPair& aOther) const {
  if (mOffset < aOther.mOffset) {
    return true;
  }

  if (mOffset == aOther.mOffset && mLen < aOther.mLen) {
    return true;
  }

  return false;
}

void ValidityPair::Merge(const ValidityPair& aOther) {
  MOZ_ASSERT(CanBeMerged(aOther));

  uint32_t offset = std::min(mOffset, aOther.mOffset);
  uint32_t end = std::max(mOffset + mLen, aOther.mOffset + aOther.mLen);

  mOffset = offset;
  mLen = end - offset;
}

void ValidityMap::Log() const {
  LOG(("ValidityMap::Log() - number of pairs: %zu", mMap.Length()));
  for (uint32_t i = 0; i < mMap.Length(); i++) {
    LOG(("    (%u, %u)", mMap[i].Offset() + 0, mMap[i].Len() + 0));
  }
}

uint32_t ValidityMap::Length() const { return mMap.Length(); }

void ValidityMap::AddPair(uint32_t aOffset, uint32_t aLen) {
  ValidityPair pair(aOffset, aLen);

  if (mMap.Length() == 0) {
    mMap.AppendElement(pair);
    return;
  }

  uint32_t pos = 0;
  for (pos = mMap.Length(); pos > 0;) {
    --pos;

    if (mMap[pos].LessThan(pair)) {
      if (mMap[pos].CanBeMerged(pair)) {
        mMap[pos].Merge(pair);
      } else {
        ++pos;
        if (pos == mMap.Length()) {
          mMap.AppendElement(pair);
        } else {
          mMap.InsertElementAt(pos, pair);
        }
      }

      break;
    }

    if (pos == 0) {
      mMap.InsertElementAt(0, pair);
    }
  }

  while (pos + 1 < mMap.Length()) {
    if (mMap[pos].CanBeMerged(mMap[pos + 1])) {
      mMap[pos].Merge(mMap[pos + 1]);
      mMap.RemoveElementAt(pos + 1);
    } else {
      break;
    }
  }
}

void ValidityMap::Clear() { mMap.Clear(); }

size_t ValidityMap::SizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mMap.ShallowSizeOfExcludingThis(mallocSizeOf);
}

ValidityPair& ValidityMap::operator[](uint32_t aIdx) {
  return mMap.ElementAt(aIdx);
}

void FreeBuffer(void* aBuf) {
#ifndef NS_FREE_PERMANENT_DATA
  if (CacheObserver::ShuttingDown()) {
    return;
  }
#endif

  free(aBuf);
}

nsresult ParseAlternativeDataInfo(const char* aInfo, int64_t* _offset,
                                  nsACString* _type) {
  mozilla::Tokenizer p(aInfo, nullptr, "/");
  uint32_t altDataVersion = 0;
  int64_t altDataOffset = -1;

  if (!p.ReadInteger(&altDataVersion) || altDataVersion != kAltDataVersion) {
    LOG(
        ("ParseAlternativeDataInfo() - altDataVersion=%u, "
         "expectedVersion=%u",
         altDataVersion, kAltDataVersion));
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (!p.CheckChar(';') || !p.ReadInteger(&altDataOffset) ||
      !p.CheckChar(',')) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (altDataOffset < 0) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (_offset) {
    *_offset = altDataOffset;
  }

  if (_type) {
    (void)p.ReadUntil(Tokenizer::Token::EndOfFile(), *_type);
  }

  return NS_OK;
}

void BuildAlternativeDataInfo(const char* aInfo, int64_t aOffset,
                              nsACString& _retval) {
  _retval.Truncate();
  _retval.AppendInt(kAltDataVersion);
  _retval.Append(';');
  _retval.AppendInt(aOffset);
  _retval.Append(',');
  _retval.Append(aInfo);
}

}  
