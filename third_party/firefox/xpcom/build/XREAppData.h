/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsXREAppData_h
#define nsXREAppData_h

#include "mozilla/StaticXREAppData.h"
#include "mozilla/UniquePtrExtensions.h"
#include "nsCOMPtr.h"
#include "nsCRTGlue.h"
#include "nsStringFwd.h"
#include "nsIFile.h"


namespace mozilla {

struct StaticXREAppData;

class XREAppData {
 public:
  XREAppData() = default;
  ~XREAppData() = default;
  XREAppData(const XREAppData& aOther) { *this = aOther; }

  explicit XREAppData(const StaticXREAppData& aOther) { *this = aOther; }

  XREAppData& operator=(const StaticXREAppData& aOther);
  XREAppData& operator=(const XREAppData& aOther);
  XREAppData& operator=(XREAppData&& aOther) = default;

  class CharPtr {
   public:
    explicit CharPtr() = default;
    explicit CharPtr(const char* v) { *this = v; }
    CharPtr(CharPtr&&) = default;
    ~CharPtr() = default;

    CharPtr& operator=(const char* v) {
      if (v) {
        mValue.reset(NS_xstrdup(v));
      } else {
        mValue = nullptr;
      }
      return *this;
    }
    CharPtr& operator=(const CharPtr& v) {
      *this = (const char*)v;
      return *this;
    }

    operator const char*() const { return mValue.get(); }

   private:
    UniqueFreePtr<const char> mValue;
  };

  nsCOMPtr<nsIFile> directory;

  CharPtr vendor;

  CharPtr name;

  CharPtr remotingName;

  CharPtr version;

  CharPtr buildID;

  CharPtr ID;

  /**
   * The copyright information to print for the -h commandline flag,
   * e.g. "Copyright (c) 2003 mozilla.org".
   */
  CharPtr copyright;

  uint32_t flags = 0;

  nsCOMPtr<nsIFile> xreDirectory;

  CharPtr minVersion;
  CharPtr maxVersion;

  CharPtr profile;

  CharPtr UAName;

  CharPtr sourceURL;

  CharPtr sourceRevision;

  CharPtr updateURL;


  static void SanitizeNameForDBus(nsACString&);
  void GetDBusAppName(nsACString&) const;
};

}  

#endif  // XREAppData_h
