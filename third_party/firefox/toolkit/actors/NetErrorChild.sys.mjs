/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.sys.mjs",
});

import { RemotePageChild } from "resource://gre/actors/RemotePageChild.sys.mjs";

export class NetErrorChild extends RemotePageChild {
  actorCreated() {
    super.actorCreated();

    const exportableFunctions = [
      "RPMGetAppBuildID",
      "RPMGetHostForDisplay",
      "RPMGetInnermostAsciiHost",
      "RPMRecordGleanEvent",
      "RPMCheckAlternateHostAvailable",
      "RPMGetHttpResponseHeader",
      "RPMIsTRROnlyFailure",
      "RPMIsFirefox",
      "RPMOpenPreferences",
      "RPMHasConnectivity",
      "RPMGetTRRSkipReason",
      "RPMGetTRRDomain",
      "RPMIsSiteSpecificTRRError",
      "RPMSetTRRDisabledLoadFlags",
      "RPMGetCurrentTRRMode",
      "RPMShowOSXLocalNetworkPermissionWarning",
    ];
    this.exportFunctions(exportableFunctions);
  }

  RPMGetHostForDisplay(document) {
    let uri = document.mozDocumentURIIfNotForErrorPages;
    return lazy.BrowserUtils.formatURIForDisplay(uri, { showWWW: true });
  }

  RPMGetInnermostAsciiHost() {
    let uri = this.contentWindow.document.mozDocumentURIIfNotForErrorPages;
    if (uri instanceof Ci.nsINestedURI) {
      uri = uri.QueryInterface(Ci.nsINestedURI).innermostURI;
    }
    return uri.asciiHost;
  }

  RPMGetAppBuildID() {
    return Services.appinfo.appBuildID;
  }

  RPMRecordGleanEvent(category, name, extra) {
  }

  RPMCheckAlternateHostAvailable() {
    const host = this.contentWindow.location.host.trim();

    const REGEXP_SINGLE_WORD = /^[^\s@:/?#]+(:\d+)?$/;
    if (!REGEXP_SINGLE_WORD.test(host)) {
      return;
    }

    let info = Services.uriFixup.forceHttpFixup(
      this.contentWindow.location.href
    );

    if (!info.fixupCreatedAlternateURI && !info.fixupChangedProtocol) {
      return;
    }

    let { displayHost, displaySpec, pathQueryRef } = info.fixedURI;

    if (pathQueryRef.endsWith("/")) {
      pathQueryRef = pathQueryRef.slice(0, pathQueryRef.length - 1);
    }

    let weakDoc = Cu.getWeakReference(this.contentWindow.document);
    let onLookupCompleteListener = {
      onLookupComplete(request, record, status) {
        let doc = weakDoc.get();
        if (!doc || !Components.isSuccessCode(status)) {
          return;
        }

        let link = doc.createElement("a");
        link.href = displaySpec;
        link.setAttribute("data-l10n-name", "website");

        let span = doc.createElement("span");
        span.id = "dns-suggestion";
        span.appendChild(link);
        doc.l10n.setAttributes(span, "neterror-dns-not-found-with-suggestion", {
          hostAndPath: displayHost + pathQueryRef,
        });

        const shortDesc = doc.getElementById("errorShortDesc");
        if (shortDesc) {
          shortDesc.textContent += " ";
          shortDesc.appendChild(span);
        } else {
          const intro =
            doc.querySelector("net-error-card")?.wrappedJSObject?.errorIntro;
          if (intro) {
            intro.after(span);
          }
        }
      },
    };

    try {
      Services.uriFixup.checkHost(
        info.fixedURI,
        onLookupCompleteListener,
        this.document.nodePrincipal.originAttributes
      );
    } catch (ex) {
    }
  }

  RPMGetHttpResponseHeader(responseHeader) {
    let channel = this.contentWindow.docShell.failedChannel;
    if (!channel) {
      return "";
    }

    let httpChannel = channel.QueryInterface(Ci.nsIHttpChannel);
    if (!httpChannel) {
      return "";
    }

    try {
      return httpChannel.getResponseHeader(responseHeader);
    } catch (e) {}

    return "";
  }

  RPMIsTRROnlyFailure() {
    let channel = this.contentWindow?.docShell?.failedChannel?.QueryInterface(
      Ci.nsIHttpChannelInternal
    );
    if (!channel) {
      return false;
    }
    return channel.effectiveTRRMode == Ci.nsIRequest.TRR_ONLY_MODE;
  }

  RPMIsFirefox() {
    return true;
  }

  RPMHasConnectivity() {
    return Services.io.connectivity;
  }

  _getTRRSkipReason() {
    let channel = this.contentWindow?.docShell?.failedChannel?.QueryInterface(
      Ci.nsIHttpChannelInternal
    );
    return channel?.trrSkipReason ?? Ci.nsITRRSkipReason.TRR_UNSET;
  }

  RPMGetTRRSkipReason() {
    let skipReason = this._getTRRSkipReason();
    return Services.dns.getTRRSkipReasonName(skipReason);
  }

  RPMGetTRRDomain() {
    return Services.dns.trrDomain;
  }

  RPMIsSiteSpecificTRRError() {
    let skipReason = this._getTRRSkipReason();
    switch (skipReason) {
      case Ci.nsITRRSkipReason.TRR_NXDOMAIN:
      case Ci.nsITRRSkipReason.TRR_RCODE_FAIL:
      case Ci.nsITRRSkipReason.TRR_NO_ANSWERS:
        return true;
    }
    return false;
  }

  RPMSetTRRDisabledLoadFlags() {
    this.contentWindow.docShell.browsingContext.defaultLoadFlags |=
      Ci.nsIRequest.LOAD_TRR_DISABLED_MODE;
  }

  RPMShowOSXLocalNetworkPermissionWarning() {
    return false;
  }
}
