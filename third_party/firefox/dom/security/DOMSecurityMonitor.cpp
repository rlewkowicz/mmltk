/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DOMSecurityMonitor.h"

#include "mozilla/BasePrincipal.h"
#include "mozilla/StaticPrefs_dom.h"
#include "nsContentUtils.h"
#include "nsIChannel.h"
#include "nsILoadInfo.h"
#include "nsIPrincipal.h"
#include "nsIURI.h"
#include "nsJSUtils.h"
#include "xpcpublic.h"

void DOMSecurityMonitor::AuditParsingOfHTMLXMLFragments(
    nsIPrincipal* aPrincipal, const nsAString& aFragment) {
  if (!aPrincipal->IsSystemPrincipal() && !aPrincipal->SchemeIs("about")) {
    return;
  }

  if (aFragment.IsEmpty()) {
    return;
  }

  auto loc = mozilla::JSCallingLocation::Get();
  if (!loc) {
    return;
  }

  if (mozilla::StaticPrefs::dom_security_skip_html_fragment_assertion()) {
    return;
  }

  static nsLiteralCString htmlFragmentAllowlist[] = {
      "chrome://global/content/elements/marquee.js"_ns,
      nsLiteralCString(
          "resource://newtab/data/content/activity-stream.bundle.js"),
      "resource://gre/modules/narrate/VoiceSelect.sys.mjs"_ns,
      "chrome://global/content/vendor/react-dom.js"_ns,
  };

  for (const nsLiteralCString& allowlistEntry : htmlFragmentAllowlist) {
    if (StringBeginsWith(loc.FileName(), allowlistEntry)) {
      return;
    }
  }

  nsAutoCString uriSpec;
  aPrincipal->GetAsciiSpec(uriSpec);

  fprintf(stderr,
          "Do not call the fragment parser (e.g innerHTML()) in chrome code "
          "or in about: pages, (uri: %s), (caller: %s, line: %d, col: %d), "
          "(fragment: %s)",
          uriSpec.get(), loc.FileName().get(), loc.mLine, loc.mColumn,
          NS_ConvertUTF16toUTF8(aFragment).get());

  xpc_DumpJSStack(true, true, false);
  MOZ_ASSERT(false);
}

void DOMSecurityMonitor::AuditUseOfJavaScriptURI(nsIChannel* aChannel) {
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
  nsCOMPtr<nsIPrincipal> loadingPrincipal = loadInfo->GetLoadingPrincipal();

  if (!loadingPrincipal) {
    return;
  }

  if (!loadingPrincipal->IsSystemPrincipal() &&
      !loadingPrincipal->SchemeIs("about")) {
    return;
  }

  MOZ_ASSERT(false,
             "Do not use javascript: URIs in chrome code or in about: pages");
}
