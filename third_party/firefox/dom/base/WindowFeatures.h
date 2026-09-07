/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_WindowFeatures_h
#define mozilla_dom_WindowFeatures_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/HashTable.h"   // mozilla::HashMap
#include "nsString.h"
#include "nsStringFwd.h"  // nsCString, nsACString, nsAutoCString, nsLiteralCString
#include "nsTStringHasher.h"  // mozilla::DefaultHasher<nsCString>

namespace mozilla::dom {

class WindowFeatures {
 public:
  WindowFeatures() = default;

  WindowFeatures(const WindowFeatures& aOther) = delete;
  WindowFeatures& operator=(const WindowFeatures& aOther) = delete;

  WindowFeatures(WindowFeatures&& aOther) = delete;
  WindowFeatures& operator=(WindowFeatures&& aOther) = delete;

  bool Tokenize(const nsACString& aFeatures);

  template <size_t N>
  bool Exists(const char (&aName)[N]) const {
    MOZ_ASSERT(IsLowerCase(aName));
    nsLiteralCString name(aName);
    return tokenizedFeatures_.has(name);
  }

  template <size_t N>
  const nsCString& Get(const char (&aName)[N]) const {
    MOZ_ASSERT(IsLowerCase(aName));
    nsLiteralCString name(aName);
    auto p = tokenizedFeatures_.lookup(name);
    MOZ_ASSERT(p.found());

    return p->value();
  }

  template <size_t N>
  int32_t GetInt(const char (&aName)[N]) const {
    const nsCString& value = Get(aName);
    return ParseIntegerWithFallback(value);
  }

  template <size_t N>
  bool GetBool(const char (&aName)[N]) const {
    const nsCString& value = Get(aName);
    return ParseBool(value);
  }

  template <size_t N>
  bool GetBoolWithDefault(const char (&aName)[N], bool aDefault,
                          bool* aPresenceFlag = nullptr) const {
    MOZ_ASSERT(IsLowerCase(aName));
    nsLiteralCString name(aName);
    auto p = tokenizedFeatures_.lookup(name);
    if (p.found()) {
      if (aPresenceFlag) {
        *aPresenceFlag = true;
      }
      return ParseBool(p->value());
    }
    return aDefault;
  }

  template <size_t N>
  void Remove(const char (&aName)[N]) {
    MOZ_ASSERT(IsLowerCase(aName));
    nsLiteralCString name(aName);
    tokenizedFeatures_.remove(name);
  }

  bool IsEmpty() const { return tokenizedFeatures_.empty(); }

  void Stringify(nsAutoCString& aOutput);

 private:
#ifdef DEBUG
  static bool IsLowerCase(const char* text);
#endif

  static int32_t ParseIntegerWithFallback(const nsCString& aValue);
  static bool ParseBool(const nsCString& aValue);

  mozilla::HashMap<nsCString, nsCString> tokenizedFeatures_;
};

}  

#endif  // #ifndef mozilla_dom_WindowFeatures_h
