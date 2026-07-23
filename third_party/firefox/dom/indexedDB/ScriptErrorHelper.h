/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_scripterrorhelper_h_
#define mozilla_dom_indexeddb_scripterrorhelper_h_

#include "nsStringFwd.h"

namespace mozilla {
struct JSCallingLocation;
}

namespace mozilla::dom::indexedDB {

class ScriptErrorHelper {
 public:
  static void Dump(const nsAString& aMessage, const JSCallingLocation&,
                   uint32_t aSeverityFlag, 
                   bool aIsChrome, uint64_t aInnerWindowID);

  static void DumpLocalizedMessage(
      const nsACString& aMessageName, const JSCallingLocation&,
      uint32_t aSeverityFlag, 
      bool aIsChrome, uint64_t aInnerWindowID);
};

}  

#endif  // mozilla_dom_indexeddb_scripterrorhelper_h_
