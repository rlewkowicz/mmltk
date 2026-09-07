/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FramingChecker_h
#define mozilla_dom_FramingChecker_h

#include "nsStringFwd.h"

class nsIDocShell;
class nsIChannel;
class nsIHttpChannel;
class nsIDocShellTreeItem;
class nsIURI;
class nsIContentSecurityPolicy;

namespace mozilla::dom {
class BrowsingContext;
}  

class FramingChecker {
 public:
  static bool CheckFrameOptions(nsIChannel* aChannel,
                                nsIContentSecurityPolicy* aCSP,
                                bool& outIsFrameCheckingSkipped);

 protected:
  struct XFOHeader {
    bool ALLOWALL = false;
    bool SAMEORIGIN = false;
    bool DENY = false;
    bool INVALID = false;
  };

  static void ReportError(const char* aMessageTag, nsIHttpChannel* aChannel,
                          nsIURI* aURI, const nsAString& aPolicy);
};

#endif /* mozilla_dom_FramingChecker_h */
