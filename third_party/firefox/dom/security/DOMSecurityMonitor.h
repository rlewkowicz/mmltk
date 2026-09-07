/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_DOMSecurityMonitor_h
#define mozilla_dom_DOMSecurityMonitor_h

#include "nsStringFwd.h"

class nsIChannel;
class nsIPrincipal;

class DOMSecurityMonitor final {
 public:
  static void AuditParsingOfHTMLXMLFragments(nsIPrincipal* aPrincipal,
                                             const nsAString& aFragment);

  static void AuditUseOfJavaScriptURI(nsIChannel* aChannel);

 private:
  DOMSecurityMonitor() = default;
  ~DOMSecurityMonitor() = default;
};

#endif /* mozilla_dom_DOMSecurityMonitor_h */
